#include "profiling/obstacle_probe.h"
#include "navigation/flow_cache.h"
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

using namespace coastal::profiling;
namespace {
struct Check {std::string label;bool passed;};std::vector<Check> checks;
void check(std::string name,bool passed){checks.push_back({std::move(name),passed});}
std::uint64_t count(const Recorder& r,Counter field){return r.snapshot().counters[std::size_t(field)];}
std::uint64_t calls(const Recorder& r,Phase field){return r.snapshot().phases[std::size_t(field)].calls;}
bool all_zero(const Snapshot& snapshot){
    for(const auto& phase:snapshot.phases)if(phase.calls||phase.total_ns||phase.max_ns)return false;
    for(auto value:snapshot.counters)if(value)return false;return true;
}
bool same_fields(const coastal::navigation::FlowCache& a,const coastal::navigation::FlowCache& b){
    const auto x=a.export_active(),y=b.export_active();
    if(x.version!=y.version||x.width!=y.width||x.height!=y.height||x.map_revision!=y.map_revision||x.active_goals!=y.active_goals)return false;
    for(auto goal:x.active_goals){const auto* left=a.find(goal);const auto* right=b.find(goal);
        if(!left||!right||left->goal!=right->goal||left->map_revision!=right->map_revision||left->distance!=right->distance)return false;}
    return true;
}
}
int main(){
    check("All preexisting phase and counter positions remain stable",std::size_t(Phase::EventDrain)==18&&std::size_t(Counter::EncodedEvents)==14);
    check("New phases and counters have exact named public keys",std::string(phase_names[std::size_t(Phase::GridProbeOverhead)])=="grid_probe_overhead"&&std::string(counter_names[std::size_t(Counter::GridRevisionOnlyUpdates)])=="grid_revision_only_updates");
    Recorder recorder;std::vector<unsigned char> grid(64,0);const auto original=grid;
    {ObstacleProbe probe(recorder,grid,0);check("Disabled profiling captures no grid bytes",probe.captured_bytes()==0);probe.finish(grid,1);}
    check("Disabled grid probe writes no clocks or counters",all_zero(recorder.snapshot()));
    check("Disabled observation does not mutate its actual grid",grid==original);
    recorder.set_enabled(true);
    {ObstacleProbe probe(recorder,grid,0);check("Enabled probe copies exactly the current grid bytes",probe.captured_bytes()==grid.size());probe.finish(grid,1);check("Completed probe releases temporary storage",probe.captured_bytes()==0);probe.finish(grid,2);}
    check("Identical blocked bytes with changed revision is revision-only",count(recorder,Counter::GridRevisionOnlyUpdates)==1&&count(recorder,Counter::GridChangedUpdates)==0&&count(recorder,Counter::GridChangedCells)==0);
    check("Capture and compare each have one explicit overhead observation",calls(recorder,Phase::GridProbeOverhead)==2);
    check("Finishing twice cannot invent another grid update",count(recorder,Counter::GridRevisionOnlyUpdates)==1);
    {ObstacleProbe probe(recorder,grid,1);probe.finish(grid,1);}
    check("A completely unchanged grid and revision is a separate diagnostic no-op",count(recorder,Counter::GridUnchangedUpdates)==1&&count(recorder,Counter::GridRevisionOnlyUpdates)==1);
    {ObstacleProbe probe(recorder,grid,1);grid[5]=1;probe.finish(grid,2);}
    check("Single physical blocked-byte change is exact",count(recorder,Counter::GridChangedUpdates)==1&&count(recorder,Counter::GridChangedCells)==1);
    const auto wall=grid;
    {ObstacleProbe probe(recorder,grid,2);grid[5]=0;grid[6]=1;probe.finish(grid,3);}
    check("Moving one blocked cell detects both bytes despite unchanged blocked population",count(recorder,Counter::GridChangedUpdates)==2&&count(recorder,Counter::GridChangedCells)==3);
    check("Probe never modifies caller history or new blocked bytes",wall[5]==1&&wall[6]==0&&grid[5]==0&&grid[6]==1);
    {ObstacleProbe probe(recorder,grid,3);grid.push_back(0);grid.push_back(1);probe.finish(grid,4);}
    check("Changed array length is not mistaken for a revision-only update",count(recorder,Counter::GridChangedUpdates)==3&&count(recorder,Counter::GridChangedCells)==5);
    {ObstacleProbe probe(recorder,grid,4);grid.pop_back();probe.finish(grid,5);}
    check("Removed grid bytes are counted without out-of-bounds reads",count(recorder,Counter::GridChangedCells)==6);
    const auto before_abort=recorder.snapshot();
    {ObstacleProbe abandoned(recorder,grid,5);}
    check("Abandoned observation cannot fabricate an accepted grid change",count(recorder,Counter::GridChangedUpdates)==before_abort.counters[std::size_t(Counter::GridChangedUpdates)]&&count(recorder,Counter::GridRevisionOnlyUpdates)==before_abort.counters[std::size_t(Counter::GridRevisionOnlyUpdates)]);
    check("Abandoned probe still accounts for capture and release overhead",calls(recorder,Phase::GridProbeOverhead)==14);
    recorder.reset();check("Observation reset clears every new counter and phase",all_zero(recorder.snapshot())&&recorder.enabled());
    const auto scoped=[&](Phase phase){Scope scope(recorder,phase);scope.stop();scope.stop();};
    for(auto phase:{Phase::ObstacleUpdate,Phase::BlockedGridBuild,Phase::FlowGridUpdate,Phase::BuildingFireCommit})scoped(phase);
    check("All four gameplay-workload observation stages are individually recorded once",calls(recorder,Phase::ObstacleUpdate)==1&&calls(recorder,Phase::BlockedGridBuild)==1&&calls(recorder,Phase::FlowGridUpdate)==1&&calls(recorder,Phase::BuildingFireCommit)==1);
    recorder.add(Counter::ObstacleUpdates,2);recorder.add(Counter::ObstacleRecords,40);recorder.add(Counter::BlockedGridCells,8192);
    recorder.add(Counter::BuildingFireCommits,3);recorder.add(Counter::BuildingFireHistoryVisited,120);recorder.add(Counter::BuildingFireLivePillboxes,9);recorder.add(Counter::BuildingFireDecisions,5);recorder.add(Counter::BuildingFireLaunches,4);
    check("Obstacle workload dimensions are counts rather than hidden timing estimates",count(recorder,Counter::ObstacleUpdates)==2&&count(recorder,Counter::ObstacleRecords)==40&&count(recorder,Counter::BlockedGridCells)==8192);
    check("Building source history live visits decisions and accepted launches remain distinct",count(recorder,Counter::BuildingFireCommits)==3&&count(recorder,Counter::BuildingFireHistoryVisited)==120&&count(recorder,Counter::BuildingFireLivePillboxes)==9&&count(recorder,Counter::BuildingFireDecisions)==5&&count(recorder,Counter::BuildingFireLaunches)==4);
    recorder.set_enabled(false);{ObstacleProbe ignored(recorder,grid,6);ignored.finish(grid,7);scoped(Phase::BuildingFireCommit);}
    check("Disabling restores zero observation writes including new scopes",all_zero(recorder.snapshot()));

    // The observed and unobserved caches receive identical updates. A third,
    // freshly configured cache always builds full fields instead of repairing
    // retained fields, so incremental reuse is checked against a fresh oracle.
    coastal::navigation::FlowCache oracle,observed;std::string error;
    std::vector<unsigned char> world(64*64,0);
    for(int z=0;z<64;++z)if(z!=20)world[z*64+32]=1;
    const bool configured=oracle.configure(64,64,world,0,error)&&observed.configure(64,64,world,0,error);
    check("Real cache fixtures accept a wall with a reachable gap",configured);
    const std::vector<int> refs{4095,4095,7};
    const bool initial=configured&&oracle.set_active(refs,error)&&observed.set_active(refs,error)&&oracle.prepare_active(error,1)&&observed.prepare_active(error,1);
    check("Both oracle and observed caches publish identical complete initial distance arrays",initial&&same_fields(oracle,observed));
    recorder.set_enabled(true);bool exact=initial;std::uint32_t revision=0;
    const auto update=[&](const std::vector<unsigned char>& replacement){
        const auto previous=observed.stats();
        const bool physical=world!=replacement;
        ObstacleProbe probe(recorder,world,revision);world=replacement;++revision;probe.finish(world,revision);
        const bool accepted=oracle.set_grid(world,revision,error)&&observed.set_grid(world,revision,error);
        coastal::navigation::FlowCache fresh;
        const bool rebuilt=fresh.configure(64,64,world,revision,error)&&fresh.set_active(refs,error)&&fresh.prepare_active(error,1);
        exact=exact&&accepted&&oracle.prepare_active(error,1)&&observed.prepare_active(error,1)&&rebuilt&&same_fields(oracle,observed)&&same_fields(fresh,observed);
        const auto after=observed.stats();
        return physical?(after.fields_built>previous.fields_built||after.fields_repaired>previous.fields_repaired):
            (after.fields_built==previous.fields_built&&after.fields_repaired==previous.fields_repaired);
    };
    check("Revision-only update retains actual cached distances without rebuilding",update(world));
    auto replacement=world;replacement[21*64+32]=0;
    check("Opening a genuine blocked cell repairs or rebuilds the actual active goal fields",update(replacement));
    check("Repeating identical topology is revision-only after a real change",update(world));
    replacement=world;replacement[21*64+32]=1;
    check("Closing the same real cell repairs or rebuilds without changing fresh oracle results",update(replacement));
    check("All compared fields goals revisions and full distances remain exact",exact&&same_fields(oracle,observed));
    check("Observed counts distinguish two true changes from two revision-only updates",count(recorder,Counter::GridChangedUpdates)==2&&count(recorder,Counter::GridChangedCells)==2&&count(recorder,Counter::GridRevisionOnlyUpdates)==2);
    check("Existing invalidation diagnostic alone would count all four cases",observed.stats().grid_invalidations==4&&oracle.stats().grid_invalidations==4);
    check("Both caches retain identical build repair counts and reference authority",observed.stats().fields_built==oracle.stats().fields_built&&observed.stats().fields_repaired==oracle.stats().fields_repaired&&observed.stats().active_references==oracle.stats().active_references&&observed.export_active().active_goals==oracle.export_active().active_goals);
    // active_goals are deduplicated; actual reference authority still counts3.
    check("Deduplicated goals and three stable references are not changed by observation",observed.export_active().active_goals==std::vector<int>({7,4095})&&observed.stats().active_references==3);

    const bool passed=std::all_of(checks.begin(),checks.end(),[](const Check& c){return c.passed;});
    std::cout<<"{\"all_passed\":"<<(passed?"true":"false")<<",\"check_count\":"<<checks.size()<<",\"checks\":[";
    for(std::size_t i=0;i<checks.size();++i){if(i)std::cout<<',';std::cout<<"{\"label\":\""<<checks[i].label<<"\",\"passed\":"<<(checks[i].passed?"true":"false")<<'}';}
    std::cout<<"]}\n";return passed?0:1;
}
