// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace coastal::macos_host {

class MixArchive {
public:
    struct Entry {
        std::uint32_t id;
        std::uint64_t size;
    };

    // Supplied by the caller for encrypted indexes. The encrypted input and the
    // output are equal-sized block spans. Only a complete decode returns true.
    using DecryptDirectory = std::function<bool(std::span<const std::uint8_t> key_block,
                                                 std::span<const std::uint8_t> input,
                                                 std::span<std::uint8_t> output)>;

    static std::optional<MixArchive> open(const std::filesystem::path& path,
                                          const DecryptDirectory& decrypt, std::string& error);
    std::optional<MixArchive> open_child(std::uint32_t id, const DecryptDirectory& decrypt,
                                          std::string& error) const;
    std::optional<Entry> find(std::uint32_t id) const noexcept;
    std::size_t entry_count() const noexcept;
    bool read(std::uint32_t id, std::uint64_t offset, std::span<std::uint8_t> output,
              std::string& error) const;

private:
    struct Impl;
    explicit MixArchive(std::shared_ptr<const Impl> index);
    std::shared_ptr<const Impl> index_;
};

}  // namespace coastal::macos_host
