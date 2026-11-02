#pragma once
#include "tick_profile.h"
#include <algorithm>
#include <new>
#include <vector>

namespace coastal::profiling {
// Read-only, exact comparison. Only enabled profiling copies grid bytes, and
// all capture/compare/release time has its own inclusive overhead phase.
// A diagnostic allocation failure must never change the game's control flow.
class ObstacleProbe {
public:
    ObstacleProbe(Recorder& recorder,const std::vector<unsigned char>& before,std::uint32_t revision)
        :recorder_(recorder.enabled()?&recorder:nullptr),revision_(revision){
        if(!recorder_)return;
        Scope overhead(*recorder_,Phase::GridProbeOverhead);
        try {before_=before;}
        catch(const std::bad_alloc&){recorder_->add(Counter::GridProbeSkipped);recorder_=nullptr;}
    }
    ~ObstacleProbe(){release();}
    ObstacleProbe(const ObstacleProbe&)=delete;
    ObstacleProbe& operator=(const ObstacleProbe&)=delete;
    void finish(const std::vector<unsigned char>& after,std::uint32_t revision){
        if(!recorder_||finished_)return;
        Scope overhead(*recorder_,Phase::GridProbeOverhead);
        const auto overlap=std::min(before_.size(),after.size());
        std::uint64_t changed=std::max(before_.size(),after.size())-overlap;
        for(std::size_t i=0;i<overlap;++i)if(before_[i]!=after[i])++changed;
        if(changed){recorder_->add(Counter::GridChangedUpdates);recorder_->add(Counter::GridChangedCells,changed);}
        else recorder_->add(revision!=revision_?Counter::GridRevisionOnlyUpdates:Counter::GridUnchangedUpdates);
        // Destroy temporary storage inside the observation-overhead timer.
        std::vector<unsigned char>().swap(before_);finished_=true;
    }
    // Diagnostics/tests only. Disabled probes allocate no grid storage.
    std::size_t captured_bytes() const {return before_.size();}
private:
    void release(){
        if(!recorder_||finished_)return;
        Scope overhead(*recorder_,Phase::GridProbeOverhead);
        std::vector<unsigned char>().swap(before_);finished_=true;
    }
    Recorder* recorder_=nullptr;
    std::vector<unsigned char> before_;
    std::uint32_t revision_=0;
    bool finished_=false;
};
}
