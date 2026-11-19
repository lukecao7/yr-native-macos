#include "macos_host/mix_archive.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

using coastal::macos_host::MixArchive;
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

constexpr std::uint32_t checksum_flag = 0x00010000;
constexpr std::uint32_t encryption_flag = 0x00020000;
constexpr std::uint8_t fixture_key = 0xa3;
constexpr std::uint8_t fixture_mask = 0x5a;
constexpr std::uint32_t low_id = 0x1020304;
constexpr std::uint32_t high_id = 0xf1234567;

struct Row {
    std::uint32_t id;
    std::uint32_t offset;
    std::uint32_t size;
};

void require(bool satisfied, std::string_view message) {
    if (!satisfied) {
        throw std::runtime_error(std::string(message));
    }
}

class FixtureDirectory {
public:
    FixtureDirectory() {
        auto pattern = (fs::temp_directory_path() / "mix-reader-test-XXXXXX").string();
        const char* created = ::mkdtemp(pattern.data());
        require(created != nullptr, "could not create fixture directory");
        path = created;
    }

    ~FixtureDirectory() {
        std::error_code error;
        fs::remove_all(path, error);
    }

    fs::path path;
};

void append_le(Bytes& bytes, std::uint32_t value, std::size_t length) {
    for (std::size_t i = 0; i < length; ++i) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
}

Bytes make_archive(std::initializer_list<Row> rows, const Bytes& payload,
                   std::optional<std::uint32_t> flags = std::nullopt) {
    Bytes bytes;
    Bytes index;
    append_le(index, static_cast<std::uint32_t>(rows.size()), 2);
    append_le(index, static_cast<std::uint32_t>(payload.size()), 4);
    for (const auto& row : rows) {
        append_le(index, row.id, 4);
        append_le(index, row.offset, 4);
        append_le(index, row.size, 4);
    }
    if (flags) {
        append_le(bytes, *flags, 4);
    }
    if (flags.value_or(0) & encryption_flag) {
        bytes.resize(bytes.size() + 80, fixture_key);
        index.resize(((index.size() + 7) / 8) * 8);
        for (auto& byte : index) {
            byte ^= fixture_mask;
        }
    }
    bytes.insert(bytes.end(), index.begin(), index.end());
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    if (flags.value_or(0) & checksum_flag) {
        bytes.resize(bytes.size() + 20, 0);
    }
    return bytes;
}

void write_archive(const fs::path& path, const Bytes& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!bytes.empty()) {
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    out.close();
    require(out.good(), "could not write archive fixture");
}

void check_layouts(const fs::path& root) {
    const auto path = root / "sample.mix";
    std::string error;
    for (const auto& flags : {std::optional<std::uint32_t>{},
                             std::optional<std::uint32_t>{0},
                             std::optional<std::uint32_t>{checksum_flag}}) {
        write_archive(path, make_archive({{high_id, 2, 3}, {low_id, 0, 2}, {12, 5, 0}},
                                         {42, 35, 99, 72, 88}, flags));
        error = "stale error";
        const auto archive = MixArchive::open(path, {}, error);
        require(archive && archive->entry_count() == 3 && error.empty(), "could not mount layout");
        const auto entry = archive->find(high_id);
        require(entry && entry->id == high_id && entry->size == 3, "incorrect entry metadata");
        require(!archive->find(31), "missing ID matched an entry");
        std::array<std::uint8_t, 2> bytes{};
        require(archive->read(high_id, 1, bytes, error) &&
                    bytes == std::array<std::uint8_t, 2>{72, 88}, "incorrect entry slice");
        require(archive->read(12, 0, {}, error), "could not read zero-length entry");
        require(!archive->read(low_id, 2, bytes, error) && !error.empty(),
                "entry allowed a cross-boundary read");
        require(bytes == std::array<std::uint8_t, 2>{72, 88},
                "out-of-range request modified the output");
        require(!archive->read(31, 0, bytes, error), "missing resource was readable");
        require(archive->read(low_id, 2, {}, error) && error.empty(),
                "empty read at the boundary failed");
    }
    write_archive(path, make_archive({}, {}));
    const auto empty = MixArchive::open(path, {}, error);
    require(empty && empty->entry_count() == 0, "could not mount the empty classic layout");
}

void check_encrypted_child(const fs::path& root) {
    const auto path = root / "nested.mix";
    const auto replacement = root / "replacement.mix";
    const auto child = make_archive({{high_id, 0, 3}, {low_id, 3, 2}},
                                    {71, 72, 73, 81, 82}, encryption_flag | checksum_flag);
    std::vector<std::size_t> calls;
    const auto decrypt = [&](std::span<const std::uint8_t> key,
                             std::span<const std::uint8_t> input,
                             std::span<std::uint8_t> output) {
        require(key.size() == 80 &&
                    std::all_of(key.begin(), key.end(), [](auto byte) { return byte == fixture_key; }),
                "incorrect callback key block");
        require(input.size() == output.size() && input.size() % 8 == 0,
                "incorrect callback block size");
        calls.push_back(input.size());
        std::transform(input.begin(), input.end(), output.begin(),
                       [](auto byte) { return byte ^ fixture_mask; });
        return true;
    };
    write_archive(replacement, child);
    std::string error;
    require(!MixArchive::open(replacement, {}, error) && error.find("decoder") != std::string::npos,
            "encrypted archive did not report the missing decoder");
    const auto decodes_head_only = [&](auto key, auto input, auto output) {
        return input.size() == 8 && decrypt(key, input, output);
    };
    require(!MixArchive::open(replacement, decodes_head_only, error) &&
                error.find("entries") != std::string::npos, "rejected entry decode was ignored");
    calls.clear();

    Bytes payload{0x11, 0x22};
    payload.insert(payload.end(), child.begin(), child.end());
    const auto child_length = static_cast<std::uint32_t>(child.size());
    write_archive(path, make_archive({{4, 0, 2}, {7, 2, child_length}}, payload));
    auto parent = MixArchive::open(path, {}, error);
    require(parent.has_value(), "could not mount parent");
    require(!parent->open_child(6, decrypt, error), "unexpected child entry");
    require(!parent->open_child(7, {}, error), "encrypted child mounted without decoder");

    // Keep using the original file across a pathname replacement and parent teardown.
    fs::rename(replacement, path);
    const auto archive = parent->open_child(7, decrypt, error);
    parent.reset();
    require(archive && archive->entry_count() == 2 && error.empty(),
            "could not mount encrypted child from the pinned file");
    require(calls == std::vector<std::size_t>{8, 24}, "unexpected encrypted block split");

    const auto check_read = [&archive](std::uint32_t id, std::array<std::uint8_t, 2> expected) {
        std::string read_error;
        for (int i = 0; i < 200; ++i) {
            std::array<std::uint8_t, 2> output{};
            if (!archive->read(id, id == high_id ? 1 : 0, output, read_error) ||
                output != expected) {
                return false;
            }
        }
        return true;
    };
    bool first = false;
    bool second = false;
    std::thread reader([&] { first = check_read(high_id, {72, 73}); });
    second = check_read(low_id, {81, 82});
    reader.join();
    require(first && second, "concurrent child reads returned the wrong bytes");

    write_archive(path, make_archive({}, {}, encryption_flag));
    calls.clear();
    const auto empty = MixArchive::open(path, decrypt, error);
    require(empty && empty->entry_count() == 0 && calls == std::vector<std::size_t>{8},
            "empty encrypted index did not decode in one block");
}

void check_invalid_archives(const fs::path& root) {
    const auto path = root / "bad.mix";
    std::string error;
    require(!MixArchive::open(path, {}, error) && !error.empty(), "missing file was opened");
    require(!MixArchive::open(root, {}, error) && !error.empty(), "directory was opened as a file");
    write_archive(path, make_archive({}, {}));
    auto invalid_path = path.native();
    invalid_path.append("\0extra", 6);
    require(!MixArchive::open(fs::path(invalid_path), {}, error), "NUL truncated the input path");
    const auto fifo = root / "input.fifo";
    require(::mkfifo(fifo.c_str(), 0600) == 0, "could not create FIFO fixture");
    require(!MixArchive::open(fifo, {}, error) && !error.empty(), "FIFO was opened as a file");

    const auto rejected = [&](const Bytes& bytes) {
        write_archive(path, bytes);
        require(!MixArchive::open(path, {}, error) && !error.empty(), "malformed archive was opened");
    };
    auto plain = make_archive({{low_id, 0, 1}}, {31}, checksum_flag);
    for (auto end = plain.cbegin(); end != plain.cend(); ++end) {
        rejected(Bytes(plain.cbegin(), end));
    }
    plain.push_back(0);
    rejected(plain);
    rejected(make_archive({{10, 0, 1}, {20, 0, 1}, {10, 0, 1}}, {31}));
    rejected(make_archive({{low_id, 1, 1}}, {31}));
    rejected(make_archive({{low_id, 0xffffffffU, 1}}, {31}));
    rejected(make_archive({{low_id, 0, 0xffffffffU}}, {31}));
    rejected(make_archive({{low_id, 0, 1}}, {31}, 0x00040000));

    auto child = make_archive({{10, 0, 1}}, {31});
    child.pop_back();
    const auto child_length = static_cast<std::uint32_t>(child.size());
    child.push_back(21);
    write_archive(path, make_archive({{4, 0, child_length}, {8, child_length, 1}}, child));
    const auto outer = MixArchive::open(path, {}, error);
    require(outer && !outer->open_child(4, {}, error) && !error.empty(),
            "child index crossed into the next outer entry");
    fs::resize_file(path, fs::file_size(path) - 1);
    std::array<std::uint8_t, 1> bytes{};
    require(!outer->read(8, 0, bytes, error) && !error.empty(),
            "short read after truncation was accepted");
}

void check_sparse_archive(const fs::path& root) {
    const auto path = root / "large.mix";
    constexpr std::uint32_t payload_length = 256 * 1024 * 1024;
    Bytes index;
    append_le(index, 1, 2);
    append_le(index, payload_length, 4);
    append_le(index, low_id, 4);
    append_le(index, payload_length - 1, 4);
    append_le(index, 1, 4);
    write_archive(path, index);
    fs::resize_file(path, index.size() + payload_length - 1);
    std::ofstream out(path, std::ios::binary | std::ios::app);
    out.put('Z');
    out.close();
    require(out.good(), "could not finish sparse file fixture");
    std::string error;
    const auto archive = MixArchive::open(path, {}, error);
    std::array<std::uint8_t, 1> output{};
    require(archive && archive->read(low_id, 0, output, error) && output.front() == 'Z',
            "large sparse archive did not yield the final byte");
}

}  // namespace

int main() {
    try {
        const FixtureDirectory fixtures;
        check_layouts(fixtures.path);
        check_encrypted_child(fixtures.path);
        check_invalid_archives(fixtures.path);
        check_sparse_archive(fixtures.path);
        std::cout << "MIX archive tests passed: layouts, encrypted indexes, nested and concurrent "
                     "reads, malformed inputs and sparse payload\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
