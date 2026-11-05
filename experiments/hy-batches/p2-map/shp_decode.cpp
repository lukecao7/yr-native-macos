#include "shp_decode.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::size_t kFileHeader = 8;
constexpr std::size_t kFrameHeader = 24;
constexpr std::size_t kResourceLimit = 64 * 1024 * 1024;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("SHP: " + message);
}

void requireRange(std::size_t offset, std::size_t count, std::size_t end) {
    if (offset > end || count > end - offset)
        fail("truncated data or invalid offset");
}

std::uint16_t u16(const Bytes& bytes, std::size_t offset) {
    requireRange(offset, 2, bytes.size());
    return static_cast<std::uint16_t>(bytes[offset] |
           (static_cast<std::uint16_t>(bytes[offset + 1]) << 8));
}

std::uint32_t u32(const Bytes& bytes, std::size_t offset) {
    requireRange(offset, 4, bytes.size());
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

Bytes load(const std::string& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) fail("cannot open " + path);
    const auto length = stream.tellg();
    if (length < 0 || static_cast<std::uint64_t>(length) > kResourceLimit)
        fail("input size exceeds 64 MiB limit");
    Bytes bytes(static_cast<std::size_t>(length));
    stream.seekg(0);
    if (!bytes.empty() &&
        !stream.read(reinterpret_cast<char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size())))
        fail("cannot read " + path);
    return bytes;
}

ShpInfo info(const Bytes& bytes) {
    requireRange(0, kFileHeader, bytes.size());
    if (u16(bytes, 0) != 0) fail("not a TS/RA2 SHP file");
    const ShpInfo out{u16(bytes, 2), u16(bytes, 4), u16(bytes, 6)};
    if (out.width == 0 || out.height == 0 || out.frameCount == 0)
        fail("zero canvas dimension or frame count");
    if (static_cast<std::size_t>(out.width) * out.height > kResourceLimit)
        fail("canvas exceeds 64 MiB limit");
    requireRange(kFileHeader,
                 static_cast<std::size_t>(out.frameCount) * kFrameHeader,
                 bytes.size());
    return out;
}
} // namespace

ShpInfo readShpInfo(const std::string& path) {
    return info(load(path));
}

DecodedFrame decodeFrame(const std::string& path, std::size_t frameIndex) {
    const Bytes bytes = load(path);
    const auto file = info(bytes);
    if (frameIndex >= file.frameCount) fail("frame index out of range");

    const std::size_t header = kFileHeader + frameIndex * kFrameHeader;
    const std::size_t x = u16(bytes, header);
    const std::size_t y = u16(bytes, header + 2);
    const std::size_t width = u16(bytes, header + 4);
    const std::size_t height = u16(bytes, header + 6);
    const std::uint32_t format = u32(bytes, header + 8);
    // Bytes 12..19 are metadata, not a dependency on a preceding frame.
    std::size_t cursor = u32(bytes, header + 20);
    if (x > file.width || width > file.width - x ||
        y > file.height || height > file.height - y)
        fail("frame rectangle extends outside canvas");

    DecodedFrame out{file.width, file.height,
                     Bytes(static_cast<std::size_t>(file.width) * file.height, 0)};
    if (cursor == 0 || width == 0 || height == 0) return out;
    if (format > 3) fail("unsupported compression " + std::to_string(format));
    const std::size_t tableEnd = kFileHeader + file.frameCount * kFrameHeader;
    if (cursor < tableEnd || cursor >= bytes.size())
        fail("frame data offset outside payload");

    // Bound reads by the next distinct payload, including out-of-order or
    // shared frame offsets. Alignment bytes after the last row are allowed.
    std::size_t dataEnd = bytes.size();
    for (std::size_t i = 0; i < file.frameCount; ++i) {
        const std::size_t next = u32(bytes, kFileHeader + i * kFrameHeader + 20);
        if (next > cursor) dataEnd = std::min(dataEnd, next);
    }

    for (std::size_t row = 0; row < height; ++row) {
        const std::size_t destination = (y + row) * file.width + x;
        if (format == 0 || format == 1) {
            requireRange(cursor, width, dataEnd);
            std::copy_n(bytes.begin() + cursor, width,
                        out.paletteIndices.begin() + destination);
            cursor += width;
            continue;
        }

        // Formats 2 and 3 prefix each row with its byte count, INCLUDING
        // the two-byte little-endian count itself.
        requireRange(cursor, 2, dataEnd);
        const std::size_t rowBytes = u16(bytes, cursor);
        if (rowBytes < 2) fail("row byte count is smaller than its header");
        requireRange(cursor, rowBytes, dataEnd);
        const std::size_t rowEnd = cursor + rowBytes;
        cursor += 2;
        if (format == 2) {
            if (rowEnd - cursor < width) fail("short uncompressed row");
            std::copy_n(bytes.begin() + cursor, width,
                        out.paletteIndices.begin() + destination);
            cursor = rowEnd;
            continue;
        }

        // Format 3: nonzero bytes are literal indices, 00 NN skips NN
        // transparent pixels. Each row starts at x, independently of all
        // other frames. Some real odd-width rows include ONE trailing zero
        // to reach an even width; consume that padding without writing it.
        const std::size_t paddedWidth = (width + 1) & ~std::size_t{1};
        std::size_t column = 0;
        while (cursor < rowEnd) {
            const std::uint8_t value = bytes[cursor++];
            if (value != 0) {
                if (column >= width) fail("literal exceeds frame row");
                out.paletteIndices[destination + column++] = value;
            } else {
                requireRange(cursor, 1, rowEnd);
                const std::size_t count = bytes[cursor++];
                if (count == 0 || count > paddedWidth - column)
                    fail("invalid transparent run length");
                column += count;
            }
        }
        // Any omitted tail remains transparent in the zero-filled canvas.
    }
    return out;
}
