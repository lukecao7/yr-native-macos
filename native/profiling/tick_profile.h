#pragma once
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include "thread_observation.h"

// Observation only. Host configuration/reset/snapshot occur between joined
// simulation steps. Each worker writes its own cache-aligned lane; no locks or
// atomics, game-state serialization, sampling RNG or scheduling changes.
namespace coastal::profiling {
enum class Phase : std::size_t {
    Step, VisionRefresh, VisionSerialize, OrderPrepare, FlowPrepare,
    WorkersWall, WorkerCalculate, TargetSample, MotionSample,
    Commit, BodyBuild, ProjectileAdvance, ImpactResolve, Shrapnel,
    LaunchSubmit, EventEncode, UnitSnapshot, ProjectileSnapshot, EventDrain,
    ObstacleUpdate, BlockedGridBuild, FlowGridUpdate, GridProbeOverhead, BuildingFireCommit,
    SourceOuterBackup, SourceGenerationPrepare, SourceGenerationJoin, SourceJournalCommit,
    SourcePrepareQualify, SourcePrepareOccupation, SourcePrepareActors, SourcePrepareBuildings,
    SourcePrepareContents, SourcePrepareQuerySnapshot, SourcePrepareQueryWorld,
    Count
};
inline constexpr std::array phase_names={
    "step", "vision_refresh", "vision_serialize", "order_prepare", "flow_prepare",
    "workers_wall", "worker_calculate", "target_sample_1in16", "motion_sample_1in16",
    "stock_commit", "body_build", "projectile_advance", "impact_resolve", "shrapnel",
    "launch_submit", "event_encode", "unit_snapshot", "projectile_snapshot", "event_drain",
    "obstacle_update", "blocked_grid_build", "flow_grid_update", "grid_probe_overhead", "building_fire_commit",
    "source_outer_backup", "source_generation_prepare", "source_generation_join", "source_journal_commit",
    "source_prepare_qualify", "source_prepare_occupation", "source_prepare_actors", "source_prepare_buildings",
    "source_prepare_contents", "source_prepare_query_snapshot", "source_prepare_query_world"
};
enum class Counter : std::size_t {
    Ticks, VisionObservers, VisionUnitTargets, VisionBuildingTargets, VisibilityBytes,
    LivingCalculated, UnitCandidates, BuildingCandidates, BodiesBuilt,
    ProjectilesAdvanced, Impacts, CollisionBodyTests, ShrapnelWorldBodies,
    NewLaunches, EncodedEvents,
    ObstacleUpdates, ObstacleRecords, BlockedGridCells, GridChangedUpdates,
    GridRevisionOnlyUpdates, GridUnchangedUpdates, GridChangedCells, GridProbeSkipped,
    FlowGridUpdateFailures, BuildingFireCommits, BuildingFireHistoryVisited,
    BuildingFireLivePillboxes, BuildingFireDecisions, BuildingFireLaunches, Count
};
inline constexpr std::array counter_names={
    "ticks", "vision_observers", "vision_unit_targets", "vision_building_targets", "visibility_bytes",
    "living_calculated", "unit_candidates", "building_candidates", "bodies_built",
    "projectiles_advanced", "impacts", "collision_body_tests", "shrapnel_world_bodies",
    "new_launches", "encoded_events",
    "obstacle_updates", "obstacle_records", "blocked_grid_cells", "grid_changed_updates",
    "grid_revision_only_updates", "grid_unchanged_updates", "grid_changed_cells", "grid_probe_skipped",
    "flow_grid_update_failures", "building_fire_commits", "building_fire_history_visited",
    "building_fire_live_pillboxes", "building_fire_decisions", "building_fire_launches"
};
inline constexpr std::size_t phase_count=std::size_t(Phase::Count);
inline constexpr std::size_t counter_count=std::size_t(Counter::Count);
inline constexpr std::size_t lane_count=5; // 0 host; 1..4 calculation workers.
static_assert(phase_names.size()==phase_count && counter_names.size()==counter_count);
struct Metric {std::uint64_t calls=0,total_ns=0,max_ns=0;};
struct Snapshot {
    bool enabled=false;
    std::array<Metric,phase_count> phases{};
    std::array<std::uint64_t,counter_count> counters{};
    std::array<ThreadMetric,lane_count> thread_metrics{};
};
class Recorder {
public:
    bool enabled() const {return enabled_;}
    void set_enabled(bool enabled){enabled_=enabled;reset();}
    void reset(){for(auto& lane:lanes_)lane=Lane{};}
    void add(Counter counter,std::uint64_t amount=1,std::size_t lane=0){
        if(enabled_ && lane<lane_count)lanes_[lane].counters[std::size_t(counter)]+=amount;
    }
    void record(Phase phase,std::uint64_t ns,std::size_t lane=0){
        if(!enabled_ || lane>=lane_count)return;
        auto& metric=lanes_[lane].phases[std::size_t(phase)];
        ++metric.calls;metric.total_ns+=ns;if(ns>metric.max_ns)metric.max_ns=ns;
    }
    Snapshot snapshot() const {
        Snapshot out;out.enabled=enabled_;
        for(std::size_t lane_index=0;lane_index<lanes_.size();++lane_index){
            const auto& lane=lanes_[lane_index];out.thread_metrics[lane_index]=lane.thread_metric;
            for(std::size_t i=0;i<phase_count;++i){const auto& source=lane.phases[i];auto& target=out.phases[i];
                target.calls+=source.calls;target.total_ns+=source.total_ns;if(source.max_ns>target.max_ns)target.max_ns=source.max_ns;
            }
            for(std::size_t i=0;i<counter_count;++i)out.counters[i]+=lane.counters[i];
        }
        return out;
    }
    ThreadCpuSample begin_thread_observation(std::size_t lane){
        if(!enabled_||lane>=lane_count)return {};
        auto& metric=lanes_[lane].thread_metric;
        if(metric.calls%thread_identity_sample_period==0)record_thread_identity(metric,observe_current_thread_identity());
        return observe_current_thread_cpu();
    }
    void end_thread_observation(std::size_t lane,ThreadCpuSample begin,ThreadCpuSample end,std::uint64_t wall_ns){
        if(enabled_&&lane<lane_count)record_thread_duration(lanes_[lane].thread_metric,begin,end,wall_ns);
    }
    // calculate() receives begin=floor(worker_index*N/4); ceiling recovers
    // that worker index even when N is not divisible by four (e.g. N=129).
    static std::size_t worker_lane(int begin,int count){
        return count>0?std::size_t((std::uint64_t(begin)*4+count-1)/count)+1:1;
    }
private:
    struct alignas(64) Lane {
        std::array<Metric,phase_count> phases{};
        std::array<std::uint64_t,counter_count> counters{};
        ThreadMetric thread_metric;
    };
    std::array<Lane,lane_count> lanes_{};
    bool enabled_=false;
};
class Scope {
public:
    Scope(Recorder& recorder,Phase phase,std::size_t lane=0,bool sample=true)
        :recorder_(recorder.enabled()&&sample?&recorder:nullptr),phase_(phase),lane_(lane){
        if(recorder_)start_=Clock::now();
    }
    ~Scope(){stop();}
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
    void stop(){
        if(!recorder_)return;
        const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start_).count();
        recorder_->record(phase_,std::uint64_t(elapsed>0?elapsed:0),lane_);recorder_=nullptr;
    }
private:
    using Clock=std::chrono::steady_clock;
    Recorder* recorder_;
    Phase phase_;
    std::size_t lane_;
    Clock::time_point start_{};
};
// Exactly one writer per existing Recorder lane. Main step0 and calculate1
// overlap; never sum them as distinct process CPU. Disabled construction does
// not read clocks, pthread metadata, process state or a lane.
class ThreadScope {
public:
    ThreadScope(Recorder& recorder,std::size_t lane)
        :recorder_(recorder.enabled()?&recorder:nullptr),lane_(lane){
        if(recorder_){cpu_start_=recorder_->begin_thread_observation(lane_);wall_start_=Clock::now();}
    }
    ~ThreadScope(){
        if(!recorder_)return;
        const auto wall=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-wall_start_).count();
        const auto cpu_end=observe_current_thread_cpu();
        recorder_->end_thread_observation(lane_,cpu_start_,cpu_end,std::uint64_t(wall>0?wall:0));
    }
    ThreadScope(const ThreadScope&)=delete;
    ThreadScope& operator=(const ThreadScope&)=delete;
private:
    using Clock=std::chrono::steady_clock;
    Recorder* recorder_;
    std::size_t lane_;
    ThreadCpuSample cpu_start_;
    Clock::time_point wall_start_{};
};

}
