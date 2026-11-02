#include "../native/navigation/reachability.h"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <queue>
#include <random>
#include <string>
#include <utility>
#include <vector>

using coastal::navigation::ConnectionPreservation;
using coastal::navigation::ReachabilityCache;
using coastal::navigation::ReachabilityPair;
namespace {
struct Check { std::string label; bool passed; };
std::vector<Check> checks;
void check(bool passed, const char* label) {
    checks.push_back({label, passed});
    if (!passed) std::cerr << "FAILED: " << label << '\n';
}
std::vector<unsigned char> oracle8(int width, int height, const std::vector<unsigned char>& grid, int start) {
    std::vector<unsigned char> seen(grid.size(), 0);
    if (start < 0 || start >= int(grid.size()) || grid[std::size_t(start)]) return seen;
    std::queue<int> work;
    work.push(start); seen[std::size_t(start)] = 1;
    while (!work.empty()) {
        const int cell = work.front(); work.pop();
        const int x = cell % width, z = cell / width;
        for (int dz = -1; dz <= 1; ++dz) for (int dx = -1; dx <= 1; ++dx) {
            const int nx = x + dx, nz = z + dz;
            if ((!dx && !dz) || nx < 0 || nx >= width || nz < 0 || nz >= height) continue;
            const int next = nz * width + nx;
            if (seen[std::size_t(next)] || grid[std::size_t(next)]) continue;
            // Independent movement-graph oracle, with explicit diagonal edges.
            if (dx && dz && (grid[std::size_t(z * width + nx)] || grid[std::size_t(nz * width + x)])) continue;
            seen[std::size_t(next)] = 1; work.push(next);
        }
    }
    return seen;
}
bool all_pairs_match(const ReachabilityCache& cache, int width, int height, const std::vector<unsigned char>& grid) {
    for (int from = 0; from < int(grid.size()); ++from) {
        const auto expected = oracle8(width, height, grid, from);
        for (int to = 0; to < int(grid.size()); ++to)
            if (cache.connected(from, to) != bool(expected[std::size_t(to)])) return false;
    }
    return true;
}
bool same_preview(const ConnectionPreservation& a, const ConnectionPreservation& b) {
    return a.preserved == b.preserved && a.required_pairs == b.required_pairs && a.broken_pairs == b.broken_pairs;
}
struct Frozen {
    int width, height;
    std::size_t components;
    std::uint64_t builds;
    std::vector<std::int32_t> labels;
};
Frozen freeze(const ReachabilityCache& cache) {
    return {cache.width(), cache.height(), cache.component_count(), cache.build_count(), cache.labels()};
}
bool unchanged(const ReachabilityCache& cache, const Frozen& old) {
    return cache.width() == old.width && cache.height() == old.height && cache.component_count() == old.components
        && cache.build_count() == old.builds && cache.labels() == old.labels;
}
void basics() {
    ReachabilityCache cache; std::string error;
    check(!cache.ready() && !cache.connected(0, 0) && cache.build_count() == 0, "Unconfigured queries are false and never build lazily");
    std::vector<unsigned char> open(12, 0);
    check(cache.update(4, 3, open, error) && error.empty() && cache.ready() && cache.component_count() == 1
        && all_pairs_match(cache, 4, 3, open), "Open rectangular grid matches every pair of the eight-neighbor oracle");
    const auto original = freeze(cache);
    for (int repeat = 0; repeat < 100; ++repeat) cache.update(4, 3, open, error);
    check(unchanged(cache, original), "Identical dimensions and bytes reuse labels despite arbitrary revision-only host calls");
    for (int repeat = 0; repeat < 100; ++repeat) (void)cache.connected(repeat % 12, (repeat + 5) % 12);
    check(unchanged(cache, original), "Repeated connected queries neither write labels nor counters");
    check(!cache.connected(-1, 0) && !cache.connected(0, -1) && !cache.connected(12, 0)
        && !cache.connected(0, std::numeric_limits<int>::max()), "Negative and out-of-grid endpoints are strictly false");
    auto split = open;
    for (int row = 0; row < 3; ++row) split[std::size_t(row * 4 + 1)] = 1;
    check(cache.update(4, 3, split, error) && cache.component_count() == 2 && !cache.connected(0, 3)
        && !cache.connected(1, 1) && all_pairs_match(cache, 4, 3, split), "Full wall splits regions and blocked self-query is false");
    const auto split_frozen = freeze(cache);
    check(cache.update(4, 3, open, error) && cache.build_count() == split_frozen.builds + 1 && cache.connected(0, 3), "Actual byte change rebuilds and reopening restores connectivity");
    const auto before_dimensions = cache.build_count();
    check(cache.update(3, 4, open, error) && cache.width() == 3 && cache.height() == 4
        && cache.build_count() == before_dimensions + 1 && all_pairs_match(cache, 3, 4, open), "Same byte vector under different dimensions builds a new correct partition");
    const auto before_invalid = freeze(cache);
    bool invalid = true;
    for (const auto& dimensions : {std::pair{0, 4}, {-1, 4}, {3, 0}, {513, 1}, {1, 513}, {std::numeric_limits<int>::max(), 2}})
        invalid = !cache.update(dimensions.first, dimensions.second, open, error) && !error.empty() && unchanged(cache, before_invalid) && invalid;
    check(invalid, "Invalid and overflow-sized dimensions reject atomically");
    auto nonbinary = open; nonbinary[3] = 2;
    check(!cache.update(3, 4, {0, 0}, error) && unchanged(cache, before_invalid)
        && !cache.update(3, 4, nonbinary, error) && unchanged(cache, before_invalid), "Wrong length and non-binary cells reject without replacing a usable cache");
    check(cache.update(3, 4, open, error) && error.empty() && unchanged(cache, before_invalid), "A valid identical grid after rejection still reuses original cached bytes");
}
void corners_and_boundaries() {
    ReachabilityCache cache; std::string error;
    const std::vector<unsigned char> diagonal{0, 1, 1, 0};
    check(cache.update(2, 2, diagonal, error) && cache.component_count() == 2 && !cache.connected(0, 3)
        && all_pairs_match(cache, 2, 2, diagonal), "Two diagonally touching open cells cannot cross two blocked corners");
    const std::vector<unsigned char> detour{0, 0, 1, 0};
    check(cache.update(2, 2, detour, error) && cache.connected(0, 3) && all_pairs_match(cache, 2, 2, detour), "Cardinal detour connects cells without granting an illegal diagonal edge");
    bool tiny = cache.update(1, 1, {0}, error) && cache.connected(0, 0);
    tiny = tiny && cache.update(1, 1, {1}, error) && !cache.connected(0, 0) && cache.component_count() == 0;
    tiny = tiny && cache.update(1, 5, {0, 0, 1, 0, 0}, error) && cache.connected(0, 1) && !cache.connected(0, 4);
    tiny = tiny && cache.update(5, 1, {0, 0, 1, 0, 0}, error) && cache.connected(3, 4) && !cache.connected(1, 3);
    check(tiny, "One-cell and one-dimensional maps have strict edge and blocking semantics");
    std::vector<unsigned char> solid(63, 1);
    check(cache.update(9, 7, solid, error) && cache.component_count() == 0
        && std::all_of(cache.labels().begin(), cache.labels().end(), [](auto label) { return label == -1; }), "All-blocked maps have no component and retain blocked sentinel labels");
    constexpr int side = 511;
    std::vector<unsigned char> maze(std::size_t(side) * side, 1);
    for (int z = 0; z < side; ++z) {
        if (z % 2 == 0) for (int x = 0; x < side; ++x) maze[std::size_t(z * side + x)] = 0;
        else maze[std::size_t(z * side + ((z / 2) % 2 == 0 ? side - 1 : 0))] = 0;
    }
    const int end = (side - 1) * side;
    check(cache.update(side, side, maze, error) && cache.component_count() == 1 && cache.connected(0, end)
        && oracle8(side, side, maze, 0)[std::size_t(end)], "A130k-cell winding corridor reaches its far end without a short-distance or queue limit");
    maze[std::size_t(255 * side)] = 1;
    check(cache.update(side, side, maze, error) && cache.component_count() == 2 && !cache.connected(0, end), "Closing one deep-maze bridge strictly disconnects the actual winding path");
    std::vector<unsigned char> checker(512U * 512U, 1);
    for (int z = 0; z < 512; ++z) for (int x = 0; x < 512; ++x)
        if ((x + z) % 2 == 0) checker[std::size_t(z * 512 + x)] = 0;
    check(cache.update(512, 512, checker, error) && cache.component_count() == 131072
        && cache.connected(0, 0) && !cache.connected(0, 513) && cache.labels().back() > 65535,
        "Maximum map supports131072 isolated components with32-bit labels and no corner leakage");
}
void previews() {
    ReachabilityCache base; std::string error;
    std::vector<unsigned char> open(25, 0), wall = open;
    for (int z = 0; z < 5; ++z) wall[std::size_t(z * 5 + 2)] = 1;
    base.update(5, 5, open, error);
    const auto original = freeze(base);
    ConnectionPreservation out;
    check(base.preserves_connections(open, {{0, 24}, {0, 20}}, out, error) && out.preserved && out.required_pairs == 2 && out.broken_pairs.empty(), "Identical candidate preserves every previously connected pair");
    check(base.preserves_connections(wall, {{0, 24}, {0, 20}, {4, 24}}, out, error)
        && !out.preserved && out.required_pairs == 3 && out.broken_pairs == std::vector<std::size_t>{0}, "Candidate wall reports only the required connection it actually cuts");
    check(unchanged(base, original) && base.update(5, 5, open, error) && unchanged(base, original), "Candidate previews leave both base labels and original blocked bytes untouched");
    base.update(5, 5, wall, error);
    check(base.preserves_connections(open, {{0, 24}, {2, 2}, {0, 20}}, out, error)
        && out.preserved && out.required_pairs == 1, "Originally disconnected or blocked pairs impose no invented new connectivity obligation");
    base.update(5, 5, open, error);
    auto blocked_endpoint = open; blocked_endpoint[0] = 1;
    check(base.preserves_connections(blocked_endpoint, {{0, 24}, {4, 20}, {0, 24}}, out, error)
        && !out.preserved && out.required_pairs == 3 && out.broken_pairs == std::vector<std::size_t>({0, 2}), "Closing a required endpoint breaks its pairs with stable duplicate indices");
    const auto frozen = freeze(base);
    const ConnectionPreservation sentinel{false, 99, {9, 7}};
    out = sentinel;
    bool rejected = !base.preserves_connections({0}, {{0, 24}}, out, error) && same_preview(out, sentinel) && unchanged(base, frozen);
    auto bad = open; bad[10] = 255;
    rejected = rejected && !base.preserves_connections(bad, {{0, 24}}, out, error) && same_preview(out, sentinel) && unchanged(base, frozen);
    rejected = rejected && !base.preserves_connections(open, {{-1, 24}}, out, error) && same_preview(out, sentinel) && unchanged(base, frozen);
    rejected = rejected && !base.preserves_connections(open, {{0, 25}}, out, error) && same_preview(out, sentinel) && unchanged(base, frozen);
    check(rejected, "Malformed candidate bytes or pair endpoints reject atomically without changing output or base");
    check(base.preserves_connections(wall, {}, out, error) && out.preserved && out.required_pairs == 0 && out.broken_pairs.empty()
        && unchanged(base, frozen), "Empty pair list validates the candidate without changing cached authority");
    ReachabilityCache empty; out = sentinel;
    check(!empty.preserves_connections(open, {}, out, error) && !error.empty() && same_preview(out, sentinel), "Unconfigured base cannot silently approve a candidate grid");
}
void deterministic_oracles() {
    std::mt19937 random(0xCCB30015U);
    ReachabilityCache cache; std::string error;
    bool equivalent = true, previews_equal = true, stable = true;
    for (int iteration = 0; iteration < 64; ++iteration) {
        const int width = 1 + int(random() % 12), height = 1 + int(random() % 12);
        std::vector<unsigned char> grid(std::size_t(width) * height);
        for (auto& cell : grid) cell = static_cast<unsigned char>(random() % 100 < 37);
        equivalent = cache.update(width, height, grid, error) && all_pairs_match(cache, width, height, grid) && equivalent;
        const auto before = freeze(cache);
        auto candidate = grid;
        for (auto& cell : candidate) if (random() % 7 == 0) cell ^= 1;
        std::vector<ReachabilityPair> pairs;
        for (int index = 0; index < 20; ++index) pairs.push_back({int(random() % grid.size()), int(random() % grid.size())});
        ConnectionPreservation actual;
        if (!cache.preserves_connections(candidate, pairs, actual, error)) { previews_equal = false; continue; }
        std::size_t required = 0; std::vector<std::size_t> broken;
        for (std::size_t index = 0; index < pairs.size(); ++index) {
            const auto& pair = pairs[index];
            if (!oracle8(width, height, grid, pair.from)[std::size_t(pair.to)]) continue;
            ++required;
            if (!oracle8(width, height, candidate, pair.from)[std::size_t(pair.to)]) broken.push_back(index);
        }
        previews_equal = previews_equal && actual.required_pairs == required && actual.broken_pairs == broken && actual.preserved == broken.empty();
        stable = stable && unchanged(cache, before);
        ReachabilityCache independent;
        stable = stable && independent.update(width, height, grid, error) && independent.labels() == cache.labels();
    }
    check(equivalent, "All pairs across64 deterministic blocked maps match fresh explicit eight-neighbor reachability");
    check(previews_equal, "All64 mixed close-open candidate batches match original and candidate independent graph oracles");
    check(stable, "Independent builds have stable integer labels and all candidate calls leave their base unchanged");
}
void report(std::ostream& stream) {
    const bool passed = std::all_of(checks.begin(), checks.end(), [](const auto& row) { return row.passed; });
    stream << "{\"all_passed\":" << (passed ? "true" : "false") << ",\"check_count\":" << checks.size() << ",\"checks\":[";
    for (std::size_t index = 0; index < checks.size(); ++index) {
        if (index) stream << ',';
        stream << "{\"label\":" << std::quoted(checks[index].label) << ",\"passed\":" << (checks[index].passed ? "true" : "false") << '}';
    }
    stream << "],\"scope\":\"Pure C++ static grid connectivity and candidate preservation only; no Godot, actor motion, occupancy, flow field, native dylib, App, GPU or FPS acceptance.\"}\n";
}
} // namespace
int main(int argc, char** argv) {
    basics(); corners_and_boundaries(); previews(); deterministic_oracles();
    if (argc == 2) {
        std::ofstream output(argv[1]);
        if (!output) { std::cerr << "Cannot write report\n"; return 2; }
        report(output);
    }
    report(std::cout);
    return std::all_of(checks.begin(), checks.end(), [](const auto& row) { return row.passed; }) ? 0 : 1;
}
