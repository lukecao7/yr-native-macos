#include <CommonCrypto/CommonDigest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

struct Entry {
    std::uint32_t id;
    std::uint32_t size;
    std::uint64_t offset; // Absolute file offset, matching the reference index.
    std::string sha256;
};

struct Archive {
    std::string format;
    std::uint32_t flags = 0;
    std::uint32_t data_size = 0;
    std::uint64_t data_start = 0;
    bool has_checksum = false;
    std::vector<Entry> entries;
};

static Bytes read_file(const fs::path& path) {
    if (!fs::is_regular_file(path)) {
        throw std::runtime_error("MIX input is not a regular file: " + path.string());
    }
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        throw std::runtime_error("cannot open MIX file: " + path.string());
    }
    const auto end = in.tellg();
    if (end < 0) {
        throw std::runtime_error("cannot determine MIX file length");
    }
    const auto length = static_cast<std::uint64_t>(end);
    if (length == 0) {
        throw std::runtime_error("empty MIX file");
    }
    Bytes bytes;
    if (length > bytes.max_size() ||
        length > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::runtime_error("MIX file is too large to read");
    }
    bytes.resize(static_cast<std::size_t>(length));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()))) {
        throw std::runtime_error("short read while reading MIX file");
    }
    return bytes;
}

static void require_bytes(const Bytes& bytes, std::size_t offset, std::size_t count) {
    if (offset > bytes.size() || count > bytes.size() - offset) {
        throw std::runtime_error("truncated MIX header/index at byte " +
                                 std::to_string(offset));
    }
}

static std::uint16_t le16(const Bytes& bytes, std::size_t offset) {
    require_bytes(bytes, offset, 2);
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(bytes[offset + 1] << 8);
}

static std::uint32_t le32(const Bytes& bytes, std::size_t offset) {
    require_bytes(bytes, offset, 4);
    std::uint32_t result = 0;
    for (unsigned i = 0; i < 4; ++i) {
        result |= static_cast<std::uint32_t>(bytes[offset + i]) << (8 * i);
    }
    return result;
}

static std::string digest(const std::uint8_t* data, std::uint32_t size) {
    std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> hash{};
    if (CC_SHA256(data, static_cast<CC_LONG>(size), hash.data()) == nullptr) {
        throw std::runtime_error("SHA-256 calculation failed");
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto byte : hash) {
        out << std::setw(2) << static_cast<unsigned>(byte);
    }
    return out.str();
}

static Archive parse(const Bytes& bytes) {
    Archive archive;
    std::size_t header_size = 6;
    std::uint16_t count = 0;

    // Classic MIX: u16 count, u32 data_size. The supplied werhdexp.mix is classic.
    // TS/RA2 MIX: u32 flags (low 16 bits zero), u16 count, u32 data_size.
    // A six-byte all-zero archive is the valid empty classic representation.
    if (le16(bytes, 0) == 0 && bytes.size() != 6) {
        require_bytes(bytes, 0, 10);
        archive.format = "ts-ra2";
        archive.flags = le32(bytes, 0);
        if ((archive.flags & 0x00020000U) != 0) {
            throw std::runtime_error("encrypted MIX is unsupported");
        }
        if ((archive.flags & ~0x00010000U) != 0) {
            throw std::runtime_error("unsupported MIX flags");
        }
        archive.has_checksum = (archive.flags & 0x00010000U) != 0;
        count = le16(bytes, 4);
        archive.data_size = le32(bytes, 6);
        header_size = 10;
    } else {
        require_bytes(bytes, 0, 6);
        archive.format = "classic";
        count = le16(bytes, 0);
        archive.data_size = le32(bytes, 2);
    }

    archive.data_start = header_size + static_cast<std::uint64_t>(count) * 12;
    if (archive.data_start > bytes.size()) {
        throw std::runtime_error("truncated MIX index: count=" + std::to_string(count) +
                                 " requires data_start=" + std::to_string(archive.data_start) +
                                 ", file_size=" + std::to_string(bytes.size()));
    }
    const std::uint64_t expected_size = archive.data_start + archive.data_size +
                                        (archive.has_checksum ? 20U : 0U);
    if (expected_size != bytes.size()) {
        throw std::runtime_error("bad MIX data length: declared file_size=" +
                                 std::to_string(expected_size) + ", actual file_size=" +
                                 std::to_string(bytes.size()));
    }

    archive.entries.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto position = header_size + i * 12;
        const auto id = le32(bytes, position);
        const auto relative_offset = le32(bytes, position + 4);
        const auto size = le32(bytes, position + 8);
        if (relative_offset > archive.data_size || size > archive.data_size - relative_offset) {
            throw std::runtime_error("entry " + std::to_string(i) + " id=" +
                                     std::to_string(id) + " out of bounds: offset=" +
                                     std::to_string(relative_offset) + " size=" +
                                     std::to_string(size) + " data_size=" +
                                     std::to_string(archive.data_size));
        }
        archive.entries.push_back({id, size, archive.data_start + relative_offset, ""});
    }

    // Only hash after every entry's range is validated. No reference JSON is read.
    for (auto& entry : archive.entries) {
        entry.sha256 = digest(bytes.data() + static_cast<std::size_t>(entry.offset), entry.size);
    }
    return archive;
}

static void check_output_path(const fs::path& input, const fs::path& output) {
    if (fs::exists(output) && fs::equivalent(input, output)) {
        throw std::runtime_error("output would overwrite MIX input: " + output.string());
    }
}

static fs::path extraction_path(const fs::path& directory, const Entry& entry) {
    std::ostringstream name;
    name << std::hex << std::setfill('0') << std::setw(8) << entry.id << ".bin";
    return directory / name.str();
}

static void extract(const Bytes& bytes, const Entry& entry, const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot open extracted resource: " + path.string());
    }
    out.write(reinterpret_cast<const char*>(bytes.data() + entry.offset), entry.size);
    out.close();
    if (!out) {
        throw std::runtime_error("failed to write extracted resource: " + path.string());
    }
}

static void write_manifest(const Archive& archive, std::size_t file_size, const fs::path& path) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot open manifest: " + path.string());
    }
    out << "{\n  \"format\": \"" << archive.format << "\",\n"
        << "  \"flags\": " << archive.flags << ",\n"
        << "  \"file_size\": " << file_size << ",\n"
        << "  \"entry_count\": " << archive.entries.size() << ",\n"
        << "  \"data_start\": " << archive.data_start << ",\n"
        << "  \"data_size\": " << archive.data_size << ",\n"
        << "  \"entries\": [\n";
    for (std::size_t i = 0; i < archive.entries.size(); ++i) {
        const auto& entry = archive.entries[i];
        out << "    {\"id\": " << entry.id << ", \"size\": " << entry.size
            << ", \"offset\": " << entry.offset << ", \"sha256\": \"" << entry.sha256
            << "\"}" << (i + 1 < archive.entries.size() ? "," : "") << '\n';
    }
    out << "  ]\n}\n";
    out.close();
    if (!out) {
        throw std::runtime_error("failed to write manifest: " + path.string());
    }
}

int main(int argc, char* argv[]) {
    if (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
        std::cout << "Usage: probe MIX [MANIFEST.json [EXTRACT_DIR]]\n"
                     "Offsets in MANIFEST.json are absolute file offsets.\n";
        return 0;
    }
    if (argc < 2 || argc > 4) {
        std::cerr << "Usage: probe MIX [MANIFEST.json [EXTRACT_DIR]]\n";
        return 2;
    }
    try {
        const fs::path input = argv[1];
        const fs::path manifest = argc >= 3 ? argv[2] : "manifest.json";
        const fs::path directory = argc >= 4 ? argv[3] : "extracted";
        const auto bytes = read_file(input);
        const auto archive = parse(bytes);
        const auto first = std::find_if(archive.entries.begin(), archive.entries.end(),
                                        [](const Entry& entry) { return entry.size != 0; });
        check_output_path(input, manifest);
        if (first != archive.entries.end()) {
            const auto path = extraction_path(directory, *first);
            check_output_path(input, path);
            if (fs::weakly_canonical(path) == fs::weakly_canonical(manifest)) {
                throw std::runtime_error("manifest and extracted resource paths collide");
            }
            extract(bytes, *first, path);
            std::cout << "extracted=" << path.string() << " size=" << first->size
                      << " sha256=" << first->sha256 << '\n';
        }
        write_manifest(archive, bytes.size(), manifest);
        std::cout << "file=" << input.string() << " bytes=" << bytes.size() << '\n'
                  << "format=" << archive.format << " flags=" << archive.flags
                  << " entry_count=" << archive.entries.size()
                  << " data_start=" << archive.data_start << " data_size=" << archive.data_size
                  << '\n';
        if (archive.has_checksum) {
            std::cout << "checksum_trailer=present (SHA-1 trailer not verified)\n";
        }
        for (std::size_t i = 0; i < archive.entries.size(); ++i) {
            const auto& entry = archive.entries[i];
            std::cout << "entry=" << i << " id=" << entry.id << " size=" << entry.size
                      << " offset=" << entry.offset << " sha256=" << entry.sha256 << '\n';
        }
        std::cout << "manifest=" << manifest.string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
