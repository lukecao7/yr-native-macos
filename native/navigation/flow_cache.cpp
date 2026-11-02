#include "flow_cache.h"
#include "packed_min_heap.h"
#include "flow_repair.h"
#include <algorithm>
#include <exception>
#include <functional>
#include <queue>
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

FlowCache::FlowCache(std::size_t budget, std::size_t maximum_refs)
    : inactive_budget_(budget), maximum_active_references_(maximum_refs) {}

bool FlowCache::configure(int width, int height,
                          const std::vector<unsigned char>& grid,
                          std::uint32_t revision, std::string& error) {
    error.clear();
    if (!valid_grid(width,height,grid,error)) return false;
    try {
        FlowCache candidate(inactive_budget_,maximum_active_references_);
        candidate.width_=width; candidate.height_=height;
        candidate.grid_=grid; candidate.revision_=revision;
        candidate.references_.assign(grid.size(),0);
        *this=std::move(candidate);
        return true;
    } catch (const std::exception& exception) {
        error=exception.what(); return false;
    }
}

bool FlowCache::set_grid(const std::vector<unsigned char>& grid,
                         std::uint32_t revision, std::string& error) {
    error.clear();
    if (!valid_grid(width_,height_,grid,error)) return false;
    if (grid==grid_) {
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
        std::vector<int> changed;
        for (std::size_t i=0;i<grid.size();++i) if (grid[i]!=grid_[i]) changed.push_back(int(i));
        if (!fields_.empty() && changed.size()<=64) {
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
        grid_.swap(replacement); revision_=revision; ++invalidations_;
        fields_.clear(); inactive_lru_.clear();
        resident_bytes_=inactive_bytes_=0;
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
        std::vector<std::size_t> counts(grid_.size(),0);
        std::vector<Handle> goals;
        std::size_t total=0;
        for (Handle handle:references) {
            if (handle==-1) continue;
            if (handle_for_goal(handle)<0 || ++total>maximum_active_references_) {
                error="active flow reference/limit is invalid"; return false;
            }
            if (counts[handle]++==0) goals.push_back(handle);
        }
        std::sort(goals.begin(),goals.end());
        if (counts==references_) return true;
        // Stage the entire LRU before changing pins. Invalid input or an
        // allocation failure cannot partially release live field references.
        std::list<Handle> new_lru;
        std::size_t new_inactive_bytes=0;
        for (Handle handle:inactive_lru_) if (counts[handle]==0) {
            new_lru.push_back(handle);
            new_inactive_bytes+=bytes(fields_.at(handle).field);
        }
        for (Handle handle:active_goals_) if (counts[handle]==0) {
            auto found=fields_.find(handle);
            if (found!=fields_.end()) {
                new_lru.push_back(handle);
                new_inactive_bytes+=bytes(found->second.field);
            }
        }
        references_.swap(counts); active_goals_.swap(goals);
        active_reference_count_=total;
        inactive_lru_.swap(new_lru); inactive_bytes_=new_inactive_bytes;
        for (auto& [handle,entry]:fields_) entry.inactive=references_[handle]==0;
        for (auto it=inactive_lru_.begin();it!=inactive_lru_.end();++it)
            fields_.at(*it).lru=it;
        trim_inactive();
        return true;
    } catch (const std::exception& exception) {
        error=exception.what(); return false;
    }
}

FlowCache::Field FlowCache::build_field(int width, int height,
                                      const std::vector<unsigned char>& grid,
                                      std::uint32_t revision, Handle goal) {
    Field result; result.goal=goal; result.map_revision=revision;
    result.distance.assign(grid.size(),-1);
    if (grid[goal]) return result;
    detail::PackedMinHeap4 queue;
    result.distance[goal]=0; queue.emplace(0,goal);
    while (!queue.empty()) {
        const auto [distance,cell]=queue.top(); queue.pop();
        if (distance!=result.distance[cell]) continue;
        const int x=cell%width,z=cell/width;
        for (int dz=-1;dz<=1;++dz) for (int dx=-1;dx<=1;++dx) {
            if ((!dx&&!dz)||x+dx<0||x+dx>=width||z+dz<0||z+dz>=height) continue;
            const int next=(z+dz)*width+x+dx;
            if (grid[next] || (dx&&dz&&(grid[z*width+x+dx]||grid[(z+dz)*width+x]))) continue;
            const std::int32_t candidate=distance+((dx&&dz)?1414:1000);
            if (result.distance[next]<0 || candidate<result.distance[next]) {
                result.distance[next]=candidate; queue.emplace(candidate,next);
            }
        }
    }
    return result;
}

std::size_t FlowCache::bytes(const Field& field) noexcept {
    return field.distance.capacity()*sizeof(std::int32_t);
}

void FlowCache::insert(Field field) {
    const Handle handle=field.goal;
    auto [it,created]=fields_.try_emplace(handle);
    if (!created) return;
    it->second.field=std::move(field);
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
        if (missing.empty()) {last_prepare_threads_=1; return true;}
        unsigned threads=std::min<unsigned>(4,std::max<unsigned>(1,requested_threads));
        threads=std::min<unsigned>(threads,unsigned(missing.size()));
        if (missing.size()<4 || grid_.size()*missing.size()<16384) threads=1;
        std::vector<Field> built(missing.size());
        std::vector<std::exception_ptr> failures(threads);
        const auto build_range=[&](unsigned worker) {
            try {
                for (std::size_t i=missing.size()*worker/threads;
                     i<missing.size()*(worker+1)/threads;++i)
                    built[i]=build_field(width_,height_,grid_,revision_,missing[i]);
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
            Field field=build_field(width_,height_,grid_,revision_,handle);
            ++builds_;
            for (int cell:cells) values.push_back(field.distance[cell]);
            // With no inactive capacity, a cold diagnostic query is temporary;
            // pinned routes always remain resident, independently of budget.
            if (reference_count(handle)>0 || bytes(field)<=inactive_budget_) insert(std::move(field));
        }
        result.swap(values);
        return true;
    } catch (const std::exception& exception) {
        error=exception.what(); return false;
    }
}

void FlowCache::trim_inactive() {
    while (inactive_bytes_>inactive_budget_ && !inactive_lru_.empty()) {
        const Handle handle=inactive_lru_.front();
        auto found=fields_.find(handle);
        const auto field_bytes=bytes(found->second.field);
        inactive_bytes_-=field_bytes; resident_bytes_-=field_bytes;
        fields_.erase(found); inactive_lru_.pop_front(); ++evictions_;
    }
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
                               std::string& error) {
    error.clear();
    if (metadata.version!=1) {error="flow metadata version is invalid"; return false;}
    FlowCache candidate(inactive_budget_,maximum_active_references_);
    if (!candidate.configure(metadata.width,metadata.height,grid,metadata.map_revision,error)
        || !candidate.set_active(references,error)) return false;
    if (metadata.active_goals!=candidate.active_goals_) {
        error="flow metadata must exactly match the sorted unique live references"; return false;
    }
    *this=std::move(candidate);
    return true;
}
}
