#pragma once
#include <cstddef>
#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

namespace coastal::navigation {

// Main-thread mutation only. prepare_active joins its private build workers
// before returning. Simulation workers may then call find() concurrently;
// no configure/set_grid/set_active/sample/prepare call may overlap that read.
class FlowCache {
public:
    using Handle = int; // The goal cell itself, not a recyclable slot index.
    struct Field {
        Handle goal = -1;
        std::uint32_t map_revision = 0;
        std::vector<std::int32_t> distance;
    };
    struct Metadata {
        std::uint32_t version = 1;
        int width = 0;
        int height = 0;
        std::uint32_t map_revision = 0;
        std::vector<Handle> active_goals; // Sorted unique union of actual refs.
    };
    struct Stats {
        std::size_t active_references = 0;
        std::size_t active_goals = 0;
        std::size_t resident_fields = 0;
        std::size_t pinned_fields = 0;
        std::size_t inactive_fields = 0;
        std::size_t resident_distance_bytes = 0;
        std::size_t pinned_distance_bytes = 0;
        std::size_t inactive_distance_bytes = 0;
        std::size_t inactive_budget_bytes = 0;
        std::size_t peak_resident_distance_bytes = 0;
        std::uint64_t fields_built = 0;
        std::uint64_t cache_hits = 0;
        std::uint64_t evictions = 0;
        std::uint64_t grid_invalidations = 0;
        std::uint64_t fields_repaired = 0;
        std::uint64_t repair_cells_invalidated = 0;
        std::uint64_t repair_cells_relaxed = 0;
        unsigned last_prepare_threads = 1;
        unsigned last_repair_threads = 1;
    };

    explicit FlowCache(std::size_t inactive_budget_bytes = 8U*1024U*1024U,
                       std::size_t maximum_active_references = 2200);
    FlowCache(const FlowCache&) = delete;
    FlowCache& operator=(const FlowCache&) = delete;
    FlowCache(FlowCache&&) noexcept = default;
    FlowCache& operator=(FlowCache&&) noexcept = default;
    bool configure(int width, int height, const std::vector<unsigned char>& grid,
                   std::uint32_t revision, std::string& error);
    bool set_grid(const std::vector<unsigned char>& grid,
                  std::uint32_t revision, std::string& error);
    Handle handle_for_goal(int goal) const noexcept;

    // -1 means no route. Duplicates represent multiple users of one field.
    // A failed ref validation leaves the previous pins and cache untouched.
    bool set_active(const std::vector<Handle>& references, std::string& error);
    // Missing active fields build independently (caller + at most 3 workers).
    // Join first, then publish in stable goal order; cache/LRU stays sequential.
    bool prepare_active(std::string& error, unsigned parallel_threads = 4);
    const Field* find(Handle handle) const noexcept;
    std::size_t reference_count(Handle handle) const noexcept;

    // Host-only exact query, including unpinned goals and zero cache budget.
    // Copies values before a temporary field is freed; no dangling view.
    bool sample(Handle handle, const std::vector<int>& cells,
                std::vector<std::int32_t>& result, std::string& error);
    void set_inactive_budget(std::size_t bytes);
    void discard_inactive();
    Stats stats() const noexcept;

    // Only authority refs are serialized. No distances, LRU, counters or
    // worker allocation policy belongs to mission authority.
    Metadata export_active() const;
    bool restore_active(const Metadata& metadata,
                        const std::vector<unsigned char>& grid,
                        const std::vector<Handle>& actual_references,
                        std::string& error);

private:
    struct Entry {
        Field field;
        bool inactive = false;
        std::list<Handle>::iterator lru;
    };
    int width_ = 0;
    int height_ = 0;
    std::uint32_t revision_ = 0;
    std::vector<unsigned char> grid_;
    std::vector<std::size_t> references_;
    std::vector<Handle> active_goals_;
    std::unordered_map<Handle, Entry> fields_;
    std::list<Handle> inactive_lru_;
    std::size_t inactive_budget_;
    std::size_t maximum_active_references_;
    std::size_t active_reference_count_ = 0;
    std::size_t resident_bytes_ = 0;
    std::size_t inactive_bytes_ = 0;
    std::size_t peak_bytes_ = 0;
    std::uint64_t builds_ = 0;
    std::uint64_t hits_ = 0;
    std::uint64_t evictions_ = 0;
    std::uint64_t invalidations_ = 0;
    std::uint64_t repairs_ = 0;
    std::uint64_t repair_invalidated_ = 0;
    std::uint64_t repair_relaxed_ = 0;
    unsigned last_prepare_threads_ = 1;
    unsigned last_repair_threads_ = 1;
    static Field build_field(int width, int height,
                             const std::vector<unsigned char>& grid,
                             std::uint32_t revision, Handle goal);
    static std::size_t bytes(const Field& field) noexcept;
    void insert(Field field);
    void trim_inactive();
    void touch(Entry& entry);
};
}
