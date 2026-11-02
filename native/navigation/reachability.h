#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace coastal::navigation {

struct ReachabilityPair {
    int from = -1;
    int to = -1;
};

struct ConnectionPreservation {
    bool preserved = true;
    std::size_t required_pairs = 0; // Pairs connected in the original grid.
    std::vector<std::size_t> broken_pairs; // Input indices, in input order.
};

// Derived STATIC navigation only. The caller supplies the exact existing
// blocked grid, including its padding. No actors, reservations, local host
// permits, movement/stance qualification, flow handles or mission state live
// here. Ordinary units may still encounter dynamic body/seat obstruction.
//
// Main-thread update only, before publishing to any readers. connected() is
// allocation-free/read-only: it never builds labels or changes counters.
// Do not overlap update/destruction with readers. No field is serialized.
class ReachabilityCache {
public:
    static constexpr int maximum_dimension = 512;

    // Identical dimensions and actual bytes reuse labels, independently of
    // any host revision counter. Rejects non-binary grids and commits only a
    // completely built candidate; failure leaves the previous cache intact.
    bool update(int width, int height,
                const std::vector<unsigned char>& blocked, std::string& error);

    bool connected(int from, int to) const noexcept;
    bool ready() const noexcept;
    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }
    std::size_t component_count() const noexcept { return components_; }
    std::uint64_t build_count() const noexcept { return builds_; }
    const std::vector<std::int32_t>& labels() const noexcept { return labels_; }

    // Candidate grid already contains the proposed obstacle(s); this module
    // does not duplicate the host's rectangle->blocked geometry rules.
    // Only pairs originally connected must remain connected. Originally
    // blocked/disconnected pairs impose no new requirement. Endpoints must
    // still be valid cell indices. The bool return indicates valid inputs/
    // completed computation, NOT preservation: inspect out.preserved.
    // Empty pairs still validate the candidate. Base cache never changes;
    // malformed input/allocation failure also leaves out untouched.
    bool preserves_connections(const std::vector<unsigned char>& candidate_blocked,
                               const std::vector<ReachabilityPair>& pairs,
                               ConnectionPreservation& out, std::string& error) const;

private:
    int width_ = 0;
    int height_ = 0;
    std::vector<unsigned char> blocked_;
    std::vector<std::int32_t> labels_;
    std::size_t components_ = 0;
    std::uint64_t builds_ = 0;

    static bool valid_grid(int width, int height,
                           const std::vector<unsigned char>& grid, std::string& error);
    static std::size_t build_labels(int width, int height,
                                    const std::vector<unsigned char>& grid,
                                    std::vector<std::int32_t>& labels);
};

} // namespace coastal::navigation
