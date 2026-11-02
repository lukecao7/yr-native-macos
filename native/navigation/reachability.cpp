#include "reachability.h"
#include <algorithm>
#include <exception>
#include <utility>

namespace coastal::navigation {

bool ReachabilityCache::valid_grid(int width, int height,
                                   const std::vector<unsigned char>& grid, std::string& error) {
    if (width < 1 || height < 1 || width > maximum_dimension || height > maximum_dimension
        || grid.size() != std::size_t(width) * std::size_t(height)) {
        error = "invalid reachability dimensions or grid length";
        return false;
    }
    if (std::any_of(grid.begin(), grid.end(), [](unsigned char cell) { return cell > 1; })) {
        error = "reachability grid must contain only zero or one";
        return false;
    }
    return true;
}

std::size_t ReachabilityCache::build_labels(int width, int height,
                                           const std::vector<unsigned char>& grid,
                                           std::vector<std::int32_t>& labels) {
    labels.assign(grid.size(), -1);
    std::vector<int> frontier;
    frontier.reserve(grid.size());
    std::int32_t component = 0;
    // The movement graph allows diagonals only when both adjoining cardinal
    // cells are clear. Every such diagonal therefore has a cardinal detour;
    // four-neighbour flood gives exactly its connectivity partition.
    for (int start = 0; start < int(grid.size()); ++start) {
        if (grid[std::size_t(start)] || labels[std::size_t(start)] >= 0) continue;
        frontier.clear();
        frontier.push_back(start);
        labels[std::size_t(start)] = component;
        for (std::size_t at = 0; at < frontier.size(); ++at) {
            const int cell = frontier[at], x = cell % width, z = cell / width;
            const int neighbors[4] = {x > 0 ? cell - 1 : -1, x + 1 < width ? cell + 1 : -1,
                                     z > 0 ? cell - width : -1, z + 1 < height ? cell + width : -1};
            for (int next : neighbors) {
                if (next < 0 || grid[std::size_t(next)] || labels[std::size_t(next)] >= 0) continue;
                labels[std::size_t(next)] = component;
                frontier.push_back(next);
            }
        }
        ++component; // <=512*512, safely within int32_t.
    }
    return std::size_t(component);
}

bool ReachabilityCache::update(int width, int height,
                               const std::vector<unsigned char>& blocked, std::string& error) {
    error.clear();
    if (!valid_grid(width, height, blocked, error)) return false;
    if (ready() && width_ == width && height_ == height && blocked_ == blocked) return true;
    try {
        auto new_grid = blocked;
        std::vector<std::int32_t> new_labels;
        const auto components = build_labels(width, height, new_grid, new_labels);
        // Everything that can allocate has completed. Publish one candidate.
        blocked_.swap(new_grid);
        labels_.swap(new_labels);
        width_ = width;
        height_ = height;
        components_ = components;
        ++builds_;
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool ReachabilityCache::ready() const noexcept {
    return width_ > 0 && height_ > 0
        && blocked_.size() == std::size_t(width_) * std::size_t(height_)
        && labels_.size() == blocked_.size();
}

bool ReachabilityCache::connected(int from, int to) const noexcept {
    if (!ready() || from < 0 || to < 0 || std::size_t(from) >= labels_.size() || std::size_t(to) >= labels_.size()) return false;
    const auto first = labels_[std::size_t(from)];
    return first >= 0 && first == labels_[std::size_t(to)];
}

bool ReachabilityCache::preserves_connections(const std::vector<unsigned char>& candidate_blocked,
                                              const std::vector<ReachabilityPair>& pairs,
                                              ConnectionPreservation& out, std::string& error) const {
    error.clear();
    if (!ready()) { error = "reachability cache is not configured"; return false; }
    if (!valid_grid(width_, height_, candidate_blocked, error)) return false;
    if (std::any_of(pairs.begin(), pairs.end(), [&](const auto& pair) {
            return pair.from < 0 || pair.to < 0 || std::size_t(pair.from) >= labels_.size() || std::size_t(pair.to) >= labels_.size();
        })) {
        error = "reachability connection endpoint is outside the grid";
        return false;
    }
    try {
        ConnectionPreservation candidate;
        std::vector<std::int32_t> new_labels;
        const bool identical = blocked_ == candidate_blocked;
        if (!identical && !pairs.empty()) build_labels(width_, height_, candidate_blocked, new_labels);
        for (std::size_t index = 0; index < pairs.size(); ++index) {
            const auto& pair = pairs[index];
            if (!connected(pair.from, pair.to)) continue;
            ++candidate.required_pairs;
            if (identical) continue;
            const auto first = new_labels[std::size_t(pair.from)];
            if (first < 0 || first != new_labels[std::size_t(pair.to)]) candidate.broken_pairs.push_back(index);
        }
        candidate.preserved = candidate.broken_pairs.empty();
        out = std::move(candidate);
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

} // namespace coastal::navigation
