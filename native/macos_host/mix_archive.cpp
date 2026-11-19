// SPDX-License-Identifier: GPL-3.0-only
#include "macos_host/mix_archive.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace coastal::macos_host {
namespace {

constexpr std::uint32_t checksum_flag = 0x00010000;
constexpr std::uint32_t encryption_flag = 0x00020000;
constexpr std::size_t header_size = 6;
constexpr std::size_t row_size = 12;
constexpr std::size_t key_size = 80;
constexpr std::size_t block_size = 8;
constexpr std::size_t checksum_size = 20;
constexpr std::size_t max_read_size = 1024 * 1024;

std::uint16_t le16(std::span<const std::uint8_t> bytes) noexcept {
    return std::uint16_t(bytes[0]) | (std::uint16_t(bytes[1]) << 8);
}

std::uint32_t le32(std::span<const std::uint8_t> bytes) noexcept {
    return std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) |
           (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
}

struct OpenFile {
    int fd = -1;

    OpenFile() = default;
    OpenFile(const OpenFile&) = delete;
    OpenFile& operator=(const OpenFile&) = delete;
    ~OpenFile() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

struct FileView {
    const OpenFile& file;
    std::uint64_t offset;
    std::uint64_t size;

    bool read_at(std::uint64_t at, std::span<std::uint8_t> output, std::string& error) const {
        if (at > size || output.size() > size - at) {
            error = "MIX read falls outside the requested range";
            return false;
        }
        while (!output.empty()) {
            const auto n = ::pread(file.fd, output.data(), std::min(output.size(), max_read_size),
                                   static_cast<off_t>(offset + at));
            if (n < 0) {
                const int code = errno;
                if (code == EINTR) {
                    continue;
                }
                error = "MIX read failed: " + std::generic_category().message(code);
                return false;
            }
            if (n == 0) {
                error = "MIX file ended before the requested range was read";
                return false;
            }
            at += static_cast<std::uint64_t>(n);
            output = output.subspan(static_cast<std::size_t>(n));
        }
        return true;
    }
};

struct IndexedEntry {
    MixArchive::Entry entry;
    std::uint64_t offset;
};

}  // namespace

struct MixArchive::Impl {
    std::shared_ptr<const OpenFile> file;
    std::vector<IndexedEntry> entries;

    static std::shared_ptr<const Impl> parse(
        std::shared_ptr<const OpenFile> file, std::uint64_t offset, std::uint64_t size,
        const DecryptDirectory& decrypt, std::string& error);

    const IndexedEntry* lookup(std::uint32_t id) const noexcept {
        const auto found = std::lower_bound(entries.begin(), entries.end(), id,
                                            [](const IndexedEntry& row, std::uint32_t key) {
                                                return row.entry.id < key;
                                            });
        return found != entries.end() && found->entry.id == id ? &*found : nullptr;
    }
};

std::shared_ptr<const MixArchive::Impl> MixArchive::Impl::parse(
    std::shared_ptr<const OpenFile> file, std::uint64_t offset, std::uint64_t size,
    const DecryptDirectory& decrypt, std::string& error) {
    const FileView view{*file, offset, size};
    std::array<std::uint8_t, 4> prefix{};
    if (!view.read_at(0, prefix, error)) {
        return {};
    }

    // A six-byte empty classic archive also starts with a zero count.
    const bool flagged = le16(prefix) == 0 && size != header_size;
    const std::uint32_t flags = flagged ? le32(prefix) : 0;
    if (flags & ~(checksum_flag | encryption_flag)) {
        error = "MIX archive has unsupported flags";
        return {};
    }

    const bool encrypted = (flags & encryption_flag) != 0;
    const auto prefix_size = flagged ? prefix.size() : 0;
    const auto index_offset = prefix_size + (encrypted ? key_size : 0);
    std::array<std::uint8_t, block_size> header{};
    std::array<std::uint8_t, key_size> key{};
    if (encrypted) {
        if (!decrypt) {
            error = "MIX archive requires an index decoder";
            return {};
        }
        std::array<std::uint8_t, block_size> cipher{};
        if (!view.read_at(prefix_size, key, error) || !view.read_at(index_offset, cipher, error)) {
            return {};
        }
        if (!decrypt(key, cipher, header)) {
            error = "MIX index header could not be decoded";
            return {};
        }
    } else if (!view.read_at(index_offset, std::span(header).first(header_size), error)) {
        return {};
    }

    const std::size_t count = le16(header);
    const std::uint64_t payload_size = le32(std::span(header).subspan(2, 4));
    const auto index_size = header_size + count * row_size;
    const auto stored_size = encrypted ? ((index_size + block_size - 1) / block_size) * block_size
                                       : index_size;
    const auto payload_offset = index_offset + stored_size;
    // Validate the trailer extent during mount. Entry reads do not hash the payload.
    const auto trailer_size = (flags & checksum_flag) ? checksum_size : 0;
    if (payload_offset + payload_size + trailer_size != size) {
        error = "MIX header, index and payload lengths do not match the archive length";
        return {};
    }

    std::vector<std::uint8_t> index(stored_size);
    const auto header_read = encrypted ? block_size : header_size;
    std::copy_n(header.begin(), header_read, index.begin());
    const auto remaining = std::span(index).subspan(header_read);
    if (!remaining.empty()) {
        if (encrypted) {
            std::vector<std::uint8_t> cipher(remaining.size());
            if (!view.read_at(index_offset + header_read, cipher, error)) {
                return {};
            }
            if (!decrypt(key, cipher, remaining)) {
                error = "MIX index entries could not be decoded";
                return {};
            }
        } else if (!view.read_at(index_offset + header_read, remaining, error)) {
            return {};
        }
    }

    auto result = std::make_shared<Impl>();
    result->file = std::move(file);
    result->entries.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto row = std::span(index).subspan(header_size + i * row_size, row_size);
        const auto id = le32(row);
        const auto relative_offset = le32(row.subspan(4, 4));
        const auto length = le32(row.subspan(8, 4));
        if (relative_offset > payload_size || length > payload_size - relative_offset) {
            error = "MIX index entry falls outside the payload";
            return {};
        }
        result->entries.push_back({{id, length}, offset + payload_offset + relative_offset});
    }
    std::sort(result->entries.begin(), result->entries.end(),
              [](const IndexedEntry& lhs, const IndexedEntry& rhs) {
                  return lhs.entry.id < rhs.entry.id;
              });
    if (std::adjacent_find(result->entries.begin(), result->entries.end(),
                           [](const IndexedEntry& lhs, const IndexedEntry& rhs) {
                               return lhs.entry.id == rhs.entry.id;
                           }) != result->entries.end()) {
        error = "MIX index contains a duplicate resource ID";
        return {};
    }
    return result;
}

MixArchive::MixArchive(std::shared_ptr<const Impl> index) : index_(std::move(index)) {}

std::optional<MixArchive> MixArchive::open(const std::filesystem::path& path,
                                           const DecryptDirectory& decrypt, std::string& error) {
    error.clear();
    if (path.native().find('\0') != std::filesystem::path::string_type::npos) {
        error = "MIX file path contains a NUL character";
        return std::nullopt;
    }
    auto file = std::make_shared<OpenFile>();
    // Nonblocking open lets us reject a FIFO before waiting on a writer.
    file->fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (file->fd < 0) {
        const int code = errno;
        error = "MIX file could not be opened: " + std::generic_category().message(code);
        return std::nullopt;
    }
    struct stat info {};
    if (::fstat(file->fd, &info) != 0) {
        const int code = errno;
        error = "MIX file could not be inspected: " + std::generic_category().message(code);
        return std::nullopt;
    }
    if (!S_ISREG(info.st_mode) || info.st_size < 0) {
        error = "MIX input must be a regular file";
        return std::nullopt;
    }
    auto index = Impl::parse(std::move(file), 0, static_cast<std::uint64_t>(info.st_size),
                             decrypt, error);
    if (!index) {
        return std::nullopt;
    }
    return MixArchive(std::move(index));
}

std::optional<MixArchive> MixArchive::open_child(std::uint32_t id, const DecryptDirectory& decrypt,
                                                  std::string& error) const {
    error.clear();
    const auto* child = index_ ? index_->lookup(id) : nullptr;
    if (!child) {
        error = "MIX child resource ID was not found";
        return std::nullopt;
    }
    auto index = Impl::parse(index_->file, child->offset, child->entry.size, decrypt, error);
    if (!index) {
        return std::nullopt;
    }
    return MixArchive(std::move(index));
}

std::optional<MixArchive::Entry> MixArchive::find(std::uint32_t id) const noexcept {
    const auto* found = index_ ? index_->lookup(id) : nullptr;
    if (!found) {
        return std::nullopt;
    }
    return found->entry;
}

std::size_t MixArchive::entry_count() const noexcept {
    return index_ ? index_->entries.size() : 0;
}

bool MixArchive::read(std::uint32_t id, std::uint64_t offset, std::span<std::uint8_t> output,
                      std::string& error) const {
    error.clear();
    const auto* found = index_ ? index_->lookup(id) : nullptr;
    if (!found) {
        error = "MIX resource ID was not found";
        return false;
    }
    return FileView{*index_->file, found->offset, found->entry.size}.read_at(offset, output, error);
}

}  // namespace coastal::macos_host
