#include "profiling/tick_profile.h"
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace coastal::profiling;
int main(){
    std::vector<std::pair<std::string,bool>> checks;
    const auto check=[&](std::string label,bool passed){checks.emplace_back(std::move(label),passed);};
    Recorder recorder;
    {Scope ignored(recorder,Phase::Step);recorder.add(Counter::Ticks);}
    check("Disabled recorder performs no measurement writes",!recorder.snapshot().enabled&&recorder.snapshot().phases[0].calls==0&&recorder.snapshot().counters[0]==0);
    recorder.set_enabled(true);
    {Scope measured(recorder,Phase::Step);measured.stop();measured.stop();}
    check("Scope commits exactly once even after manual stop",recorder.snapshot().phases[0].calls==1);
    {Scope excluded(recorder,Phase::TargetSample,1,false);}
    check("Unselected sample does not acquire a measurement",recorder.snapshot().phases[std::size_t(Phase::TargetSample)].calls==0);
    bool lanes=true;
    for(int count:{128,129,130,131,1024,2047,2200})for(int worker=0;worker<4;++worker)
        lanes=lanes&&Recorder::worker_lane(count*worker/4,count)==std::size_t(worker+1);
    check("Uneven worker ranges map to four distinct writable lanes",lanes);
    std::vector<std::thread> workers;
    for(std::size_t lane=1;lane<=4;++lane)workers.emplace_back([&recorder,lane]{
        for(int iteration=0;iteration<10000;++iteration){recorder.record(Phase::WorkerCalculate,37+lane,lane);recorder.add(Counter::LivingCalculated,1,lane);}
    });
    recorder.add(Counter::Ticks,30);
    for(auto& worker:workers)worker.join();
    const auto result=recorder.snapshot();const auto& calculation=result.phases[std::size_t(Phase::WorkerCalculate)];
    check("Joined snapshot merges all four worker counts without lost updates",calculation.calls==40000&&result.counters[std::size_t(Counter::LivingCalculated)]==40000&&result.counters[0]==30);
    check("CPU sums and maximum retain exact lane measurements",calculation.total_ns==1580000&&calculation.max_ns==41);
    recorder.reset();const auto empty=recorder.snapshot();
    check("Reset clears observations while retaining the enabled switch",empty.enabled&&empty.phases[0].calls==0&&empty.phases[std::size_t(Phase::WorkerCalculate)].total_ns==0&&empty.counters[0]==0);
    recorder.set_enabled(false);recorder.record(Phase::Step,1);recorder.add(Counter::Ticks);
    check("Disabling clears the interval and resumes zero-write observation",!recorder.snapshot().enabled&&recorder.snapshot().phases[0].calls==0&&recorder.snapshot().counters[0]==0);
    bool passed=true;std::cout<<"{\"checks\":[";
    for(std::size_t i=0;i<checks.size();++i){const auto& [label,ok]=checks[i];passed=passed&&ok;
        if(i)std::cout<<",";std::cout<<"{\"label\":\""<<label<<"\",\"passed\":"<<(ok?"true":"false")<<"}";
    }
    std::cout<<"],\"check_count\":"<<checks.size()<<",\"all_passed\":"<<(passed?"true":"false")<<"}\n";
    return passed?0:1;
}
