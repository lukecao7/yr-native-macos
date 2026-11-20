#include "flow_cache.h"
#include "packed_min_heap.h"
#include "flow_repair.h"
#include <algorithm>
#include <exception>
#include <iterator>
#include <tuple>
#include <stdexcept>
#include <thread>
#include <utility>

namespace coastal::navigation {
namespace {
bool valid_grid(int width, int height, const std::vector<unsigned char>& grid,
                std::string& error) {
    if (width<8 || height<8 || width>512 || height>512
        || grid.size()!=std::size_t(width)*std::size_t(height)) {
        error="flow grid dimensions/length are invalid"; return false;
    }
    if (std::any_of(grid.begin(),grid.end(),[](unsigned char cell){return cell>1;})) {
        error="flow grid must contain only zero/one cells"; return false;
    }
    return true;
}
}

FlowCache::FlowCache(std::size_t budget, std::size_t maximum_refs, std::size_t distance_budget)
    : inactive_budget_(budget), maximum_active_references_(maximum_refs),
      distance_budget_(distance_budget) {}

bool FlowCache::configure(int width, int height,
                          const std::vector<unsigned char>& grid,
                          std::uint32_t revision, std::string& error,
                          const std::vector<unsigned char>& edges) {
    error.clear();
    if (!valid_grid(width,height,grid,error)) return false;
    if (!edges.empty() && edges.size()!=grid.size()) {
        error="flow adjacency does not match its grid"; return false;
    }
    if (grid.size() > distance_budget_ / sizeof(std::int32_t)) {
        error = "flow distance budget cannot hold one field for this grid";
        return false;
    }
    try {
        FlowCache candidate(inactive_budget_, maximum_active_references_, distance_budget_);
        candidate.width_=width; candidate.height_=height;
        candidate.grid_=grid; candidate.revision_=revision;
        candidate.edges_=edges;
        candidate.references_.assign(grid.size(),0);
        *this=std::move(candidate);
        return true;
    } catch (const std::exception& exception) {
        error=exception.what(); return false;
    }
}

bool FlowCache::set_grid(const std::vector<unsigned char>& grid,
                         std::uint32_t revision, std::string& error,
                         const std::vector<unsigned char>& edges) {
    error.clear();
    if (!valid_grid(width_,height_,grid,error)) return false;
    if (!edges.empty() && edges.size()!=grid.size()) {
        error="flow adjacency does not match its grid"; return false;
    }
    if (grid==grid_ && edges==edges_) {
        // An obstacle list may change without changing any blocked cell.
        // Distances are still exact; update the diagnostic revision only.
        if (revision_!=revision) {
            revision_=revision; ++invalidations_;
            for (auto& [goal,entry]:fields_) {
                (void)goal; entry.field.map_revision=revision;
            }
        }
        return true;
    }
    try {
        auto replacement=grid; // Complete allocation before invalidation.
        auto replacement_edges=edges;
        std::vector<int> changed;
        for (std::size_t i=0;i<grid.size();++i) if (grid[i]!=grid_[i]) changed.push_back(int(i));
        // Repair retains old arrays until every candidate is ready. Rebuild
        // from the new topology when both generations would exceed the cap.
        if (edges.empty() && edges_.empty() && !fields_.empty() && changed.size() <= 64 && resident_bytes_ <= distance_budget_ - resident_bytes_) {
            std::vector<int> endpoints;
            for (int cell:changed) {
                const int x=cell%width_,z=cell/width_;
                for (int dz=-1;dz<=1;++dz) for (int dx=-1;dx<=1;++dx) {
                    if (x+dx>=0 && x+dx<width_ && z+dz>=0 && z+dz<height_)
                        endpoints.push_back((z+dz)*width_+x+dx);
                }
            }
            std::sort(endpoints.begin(),endpoints.end());
            endpoints.erase(std::unique(endpoints.begin(),endpoints.end()),endpoints.end());
            std::vector<Handle> goals;
            for (const auto& [goal,entry]:fields_) {(void)entry;goals.push_back(goal);}
            std::sort(goals.begin(),goals.end());
            std::vector<const Field*> originals;
            originals.reserve(goals.size());
            for (Handle goal:goals) originals.push_back(&fields_.at(goal).field);
            record_reserved(resident_bytes_);
            std::vector<Field> candidates(goals.size());
            std::vector<detail::RepairWork> work(goals.size());
            const unsigned threads=goals.size()<4 || grid.size()*goals.size()<16384
                ?1:std::min<unsigned>(4,unsigned(goals.size()));
            std::vector<std::exception_ptr> failures(threads);
            const auto repair_range=[&](unsigned worker) {
                try {
                    for (std::size_t i=goals.size()*worker/threads;i<goals.size()*(worker+1)/threads;++i) {
                        candidates[i]=*originals[i];
                        candidates[i].map_revision=revision;
                        work[i]=detail::repair_flow_field(width_,height_,replacement,endpoints,
                                                         goals[i],candidates[i].distance);
                    }
                } catch (...) {failures[worker]=std::current_exception();}
            };
            std::vector<std::thread> workers;
            try {
                for (unsigned worker=1;worker<threads;++worker) workers.emplace_back(repair_range,worker);
            } catch (...) {
                for (auto& worker:workers) worker.join();
                throw;
            }
            repair_range(0);
            for (auto& worker:workers) worker.join();
            for (const auto& failure:failures) if (failure) std::rethrow_exception(failure);
            // All allocations/worker work succeeded. Publish only by noexcept
            // swaps, retaining stable handles, entry addresses, pins and LRU.
            grid_.swap(replacement);revision_=revision;++invalidations_;
            for (std::size_t i=0;i<goals.size();++i) {
                auto& entry=fields_.at(goals[i]);
                const auto before=bytes(entry.field),after=bytes(candidates[i]);
                resident_bytes_=resident_bytes_-before+after;
                if (entry.inactive) inactive_bytes_=inactive_bytes_-before+after;
                std::swap(entry.field,candidates[i]);
                ++repairs_;repair_invalidated_+=work[i].invalidated;repair_relaxed_+=work[i].relaxed;
            }
            peak_bytes_=std::max(peak_bytes_,resident_bytes_);last_repair_threads_=threads;
            return true;
        }
        grid_.swap(replacement); edges_.swap(replacement_edges); revision_=revision; ++invalidations_;
        fields_.clear(); inactive_lru_.clear();
        resident_bytes_=inactive_bytes_=0;
        last_repair_threads_ = 1;
        // Stable handles and actual reference counts survive a grid revision.
        return true;
    } catch (const std::exception& exception) {
        error=exception.what(); return false;
    }
}

FlowCache::Handle FlowCache::handle_for_goal(int goal) const noexcept {
    return goal>=0 && std::size_t(goal)<grid_.size()?goal:-1;
}

bool FlowCache::set_active(const std::vector<Handle>& references,
                           std::string& error) {
    error.clear();
    if (grid_.empty()) {error="flow grid is not configured"; return false;}
    try {
        std::vector<Handle> sorted;
        sorted.reserve(std::min(references.size(), maximum_active_references_));
        for (Handle handle : references) {
            if (handle == -1) continue;
            if (handle_for_goal(handle) < 0 || sorted.size() >= maximum_active_references_) {
                error = "active flow reference/limit is invalid";
                return false;
            }
            sorted.push_back(handle);
        }
        std::sort(sorted.begin(), sorted.end());
        std::vector<Handle> goals;
        std::vector<std::size_t> counts;
        goals.reserve(sorted.size());
        counts.reserve(sorted.size());
        for (const auto handle : sorted) {
            if (goals.empty() || goals.back() != handle) {
                goals.push_back(handle);
                counts.push_back(0);
            }
            ++counts.back();
        }

        if (goals == active_goals_) {
            for (std::size_t i = 0; i < goals.size(); ++i) {
                references_[goals[i]] = counts[i];
            }
            active_reference_count_ = sorted.size();
            return true;
        }

        // Stage the LRU before changing the live references. Only the active
        // handles are touched, even with a sparse 512-square map.
        std::list<Handle> new_lru;
        std::size_t new_inactive_bytes = 0;
        const auto mark_inactive = [&](Handle handle) {
            if (std::binary_search(goals.begin(), goals.end(), handle)) return;
            auto found = fields_.find(handle);
            if (found == fields_.end()) return;
            new_lru.push_back(handle);
            new_inactive_bytes += bytes(found->second.field);
        };
        for (Handle handle : inactive_lru_) mark_inactive(handle);
        for (Handle handle : active_goals_) mark_inactive(handle);

        for (Handle handle : active_goals_) references_[handle] = 0;
        for (std::size_t i = 0; i < goals.size(); ++i) {
            references_[goals[i]] = counts[i];
            auto found = fields_.find(goals[i]);
            if (found != fields_.end() && found->second.inactive) {
                found->second.generation = ++generation_;
            }
        }
        active_goals_.swap(goals);
        active_reference_count_ = sorted.size();
        inactive_lru_.swap(new_lru);
        inactive_bytes_ = new_inactive_bytes;
        for (auto& [handle, entry] : fields_) entry.inactive = references_[handle] == 0;
        for (auto it = inactive_lru_.begin(); it != inactive_lru_.end(); ++it) {
            fields_.at(*it).lru = it;
        }
        trim_inactive();
        return true;
    } catch (const std::exception& exception) {
        error=exception.what(); return false;
    }
}

FlowCache::Field FlowCache::build_field(int width, int height,
                                      const std::vector<unsigned char>& grid,
                                      std::uint32_t revision, Handle goal,
                                      const std::vector<unsigned char>& edges) {
    Field result; result.goal=goal; result.map_revision=revision;
    result.distance = std::vector<std::int32_t>(grid.size(), -1);
    if (grid[goal]) return result;
    detail::PackedMinHeap4 queue;
    result.distance[goal]=0; queue.emplace(0,goal);
    while (!queue.empty()) {
        const auto item=queue.top(); queue.pop();
        const auto distance=item.first;
        const auto cell=item.second;
        if (distance!=result.distance[cell]) continue;
        detail::for_flow_neighbour(width,height,grid,cell,[&](int next,std::int32_t edge) {
            const std::int32_t candidate=distance+edge;
            if (result.distance[next]<0 || candidate<result.distance[next]) {
                result.distance[next]=candidate; queue.emplace(candidate,next);
            }
        },edges);
    }
    return result;
}

std::size_t FlowCache::bytes(const Field& field) noexcept {
    return field.distance.capacity()*sizeof(std::int32_t);
}

std::size_t FlowCache::field_bytes() const noexcept {
    return grid_.size() * sizeof(std::int32_t);
}

void FlowCache::record_reserved(std::size_t extra) noexcept {
    peak_total_bytes_ = std::max(peak_total_bytes_, resident_bytes_ + extra);
}

void FlowCache::insert(Field field) {
    const Handle handle=field.goal;
    auto [it,created]=fields_.try_emplace(handle);
    if (!created) return;
    it->second.field=std::move(field);
    it->second.generation = ++generation_;
    const auto field_bytes=bytes(it->second.field);
    if (references_[handle]==0) {
        try {inactive_lru_.push_back(handle);}
        catch (...) {fields_.erase(it); throw;}
        it->second.inactive=true;
        it->second.lru=std::prev(inactive_lru_.end());
        inactive_bytes_+=field_bytes;
    }
    resident_bytes_+=field_bytes;
    peak_bytes_=std::max(peak_bytes_,resident_bytes_);
    record_reserved(0);
    trim_inactive();
}

bool FlowCache::prepare_active(std::string& error, unsigned requested_threads) {
    error.clear();
    if (grid_.empty()) {error="flow grid is not configured"; return false;}
    try {
        std::vector<Handle> missing;
        for (Handle handle:active_goals_) {
            if (!find(handle)) missing.push_back(handle);
            else ++hits_;
        }
        const auto bytes_per_field = field_bytes();
        while (!inactive_lru_.empty() &&
               missing.size() > (distance_budget_ - resident_bytes_) / bytes_per_field) {
            evict_inactive();
        }
        missing.resize(std::min(missing.size(), (distance_budget_ - resident_bytes_) / bytes_per_field));
        if (missing.empty()) {last_prepare_threads_=1; return true;}

        record_reserved(missing.size() * bytes_per_field);
        unsigned threads=std::min<unsigned>(4,std::max<unsigned>(1,requested_threads));
        threads=std::min<unsigned>(threads,unsigned(missing.size()));
        if (missing.size()<4 || grid_.size()*missing.size()<16384) threads=1;
        std::vector<Field> built(missing.size());
        std::vector<std::exception_ptr> failures(threads);
        const auto build_range=[&](unsigned worker) {
            try {
                for (std::size_t i=missing.size()*worker/threads;
                     i<missing.size()*(worker+1)/threads;++i)
                    built[i]=build_field(width_,height_,grid_,revision_,missing[i],edges_);
            } catch (...) {failures[worker]=std::current_exception();}
        };
        std::vector<std::thread> workers;
        try {
            for (unsigned worker=1;worker<threads;++worker) workers.emplace_back(build_range,worker);
        } catch (...) {
            for (auto& worker:workers) worker.join();
            throw;
        }
        build_range(0);
        for (auto& worker:workers) worker.join();
        for (const auto& failure:failures) if (failure) std::rethrow_exception(failure);
        // Workers have finished. Only this caller mutates the map, counters,
        // eviction order or published fields; goal order is deterministic.
        last_prepare_threads_=threads;
        for (auto& field:built) {insert(std::move(field)); ++builds_;}
        return true;
    } catch (const std::exception& exception) {
        error=exception.what(); return false;
    }
}

const FlowCache::Field* FlowCache::find(Handle handle) const noexcept {
    const auto found=fields_.find(handle);
    return found!=fields_.end() && found->second.field.map_revision==revision_
        ?&found->second.field:nullptr;
}

std::size_t FlowCache::reference_count(Handle handle) const noexcept {
    return handle_for_goal(handle)>=0?references_[handle]:0;
}

void FlowCache::touch(Entry& entry) {
    entry.generation = ++generation_;
    if (entry.inactive) inactive_lru_.splice(inactive_lru_.end(),inactive_lru_,entry.lru);
}

bool FlowCache::sample(Handle handle, const std::vector<int>& cells,
                       std::vector<std::int32_t>& result, std::string& error) {
    error.clear();
    if (handle_for_goal(handle)<0 || std::any_of(cells.begin(),cells.end(),[&](int cell){return handle_for_goal(cell)<0;})) {
        error="flow sample handle/cell is invalid"; return false;
    }
    try {
        std::vector<std::int32_t> values; values.reserve(cells.size());
        if (cells.empty()) {result.swap(values); return true;}
        auto found=fields_.find(handle);
        if (found!=fields_.end()) {
            ++hits_; touch(found->second);
            for (int cell:cells) values.push_back(found->second.field.distance[cell]);
        } else {
            const auto needed = field_bytes();
            while (needed > distance_budget_ - resident_bytes_) {
                if (inactive_lru_.empty()) evict_oldest_active();
                else evict_inactive();
            }
            record_reserved(needed);
            Field field=build_field(width_,height_,grid_,revision_,handle,edges_);
            ++builds_;
            for (int cell:cells) values.push_back(field.distance[cell]);
            // With no inactive capacity a cold diagnostic query is temporary.
            if (reference_count(handle)>0 || bytes(field)<=inactive_budget_) insert(std::move(field));
        }
        result.swap(values);
        return true;
    } catch (const std::exception& exception) {
        error=exception.what(); return false;
    }
}

void FlowCache::evict_inactive() {
    const Handle handle = inactive_lru_.front();
    auto found = fields_.find(handle);
    const auto field_cost = bytes(found->second.field);
    inactive_bytes_ -= field_cost;
    resident_bytes_ -= field_cost;
    fields_.erase(found);
    inactive_lru_.pop_front();
    ++evictions_;
}

void FlowCache::evict_oldest_active() {
    // Inactive entries are evicted first. A generation wraps only after
    // 2^64 touches, and a tied generation always uses the same handle order.
    const auto oldest = std::min_element(fields_.begin(), fields_.end(),
        [](const auto& a, const auto& b) {
            return std::tie(a.second.generation, a.first) < std::tie(b.second.generation, b.first);
        });
    resident_bytes_ -= bytes(oldest->second.field);
    fields_.erase(oldest);
    ++evictions_;
}

void FlowCache::trim_inactive() {
    while (inactive_bytes_ > inactive_budget_) evict_inactive();
}

void FlowCache::set_inactive_budget(std::size_t budget) {
    inactive_budget_=budget; trim_inactive();
}
void FlowCache::discard_inactive() {
    const auto budget=inactive_budget_; inactive_budget_=0;
    trim_inactive(); inactive_budget_=budget;
}

FlowCache::Stats FlowCache::stats() const noexcept {
    Stats out;
    out.active_references=active_reference_count_; out.active_goals=active_goals_.size();
    out.resident_fields=fields_.size(); out.inactive_fields=inactive_lru_.size();
    out.pinned_fields=out.resident_fields-out.inactive_fields;
    out.resident_distance_bytes=resident_bytes_;
    out.inactive_distance_bytes=inactive_bytes_;
    out.pinned_distance_bytes=resident_bytes_-inactive_bytes_;
    out.inactive_budget_bytes=inactive_budget_; out.peak_resident_distance_bytes=peak_bytes_;
    out.distance_budget_bytes = distance_budget_;
    out.peak_total_distance_bytes = peak_total_bytes_;
    out.uncached_active_goals = out.active_goals - out.pinned_fields;
    out.fields_built=builds_; out.cache_hits=hits_; out.evictions=evictions_;
    out.grid_invalidations=invalidations_; out.last_prepare_threads=last_prepare_threads_;
    out.fields_repaired=repairs_;out.repair_cells_invalidated=repair_invalidated_;
    out.repair_cells_relaxed=repair_relaxed_;out.last_repair_threads=last_repair_threads_;
    return out;
}

FlowCache::Metadata FlowCache::export_active() const {
    Metadata result; result.width=width_; result.height=height_;
    result.map_revision=revision_; result.active_goals=active_goals_;
    return result;
}

bool FlowCache::restore_active(const Metadata& metadata,
                               const std::vector<unsigned char>& grid,
                               const std::vector<Handle>& references,
                               std::string& error,
                               const std::vector<unsigned char>& edges) {
    error.clear();
    if (metadata.version!=1) {error="flow metadata version is invalid"; return false;}
    FlowCache candidate(inactive_budget_, maximum_active_references_, distance_budget_);
    if (!candidate.configure(metadata.width,metadata.height,grid,metadata.map_revision,error,edges)
        || !candidate.set_active(references,error)) return false;
    if (metadata.active_goals!=candidate.active_goals_) {
        error="flow metadata must exactly match the sorted unique live references"; return false;
    }
    *this=std::move(candidate);
    return true;
}
}
