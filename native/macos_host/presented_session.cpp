// SPDX-License-Identifier: GPL-3.0-only
#include "presented_session.h"
#include "rts_3d/scene.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif

namespace coastal::macos_host {
namespace host=coastal::macos_host;
namespace {
void configure_producer_qos()noexcept{
#if defined(__APPLE__)
    int before_relative=0,after_relative=0;
    qos_class_t before=QOS_CLASS_UNSPECIFIED,after=QOS_CLASS_UNSPECIFIED;
    const int read_before=pthread_get_qos_class_np(pthread_self(),&before,&before_relative);
    const bool attempted=before!=QOS_CLASS_USER_INTERACTIVE;
    const int result=attempted?pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED,0):0;
    const int read_after=pthread_get_qos_class_np(pthread_self(),&after,&after_relative);
    try{
        std::cout<<"COASTAL_PRODUCER_QOS {\"thread\":\"producer\",\"beforeClass\":"<<unsigned(before)
            <<",\"beforeRelative\":"<<before_relative<<",\"requestedClass\":"<<unsigned(QOS_CLASS_USER_INITIATED)
            <<",\"setAttempted\":"<<(attempted?"true":"false")<<",\"setResult\":"<<result
            <<",\"afterClass\":"<<unsigned(after)<<",\"afterRelative\":"<<after_relative
            <<",\"readBeforeResult\":"<<read_before<<",\"readAfterResult\":"<<read_after<<"}\n";
    }catch(...){}
#endif
}
struct ReadyHandler {
    explicit ReadyHandler(host::Session::FrameReadyHandler callback):callback(std::move(callback)){}
    const host::Session::FrameReadyHandler callback;
    std::atomic<bool> active{true};
};
struct Packet {
    host::FrameCommit commit{};
    std::vector<std::uint8_t> pixels;
    std::optional<coastal::rts::view3d::SceneFrame> scene;
};
class PresentedSession final:public host::Session {
public:
    explicit PresentedSession(std::unique_ptr<host::Session> session):session_(std::move(session)),
        viewport_(session_->viewport()),interval_(session_->frame_interval()),terrain_(session_->terrain_for_metal()){}
    ~PresentedSession()override{
        set_frame_ready_handler({});
        {std::lock_guard lock(mutex_);stopping_=true;}
        condition_.notify_all();if(producer_.joinable())producer_.join();
        // Diagnostic output must never make destruction terminate the host.
        try{
            std::sort(produce_ns_.begin(),produce_ns_.end());
            const auto percentile=[&](std::size_t percent){return produce_ns_.empty()?std::uint64_t(0):produce_ns_[(produce_ns_.size()*percent+99)/100-1];};
            std::cout<<"COASTAL_PIPELINE {\"produced\":"<<produced_<<",\"delivered\":"<<delivered_<<",\"emptyPolls\":"<<empty_polls_
                <<",\"fullQueueWaits\":"<<full_waits_<<",\"packetAllocations\":"<<pool_.size()<<",\"peakReady\":"<<peak_ready_
                <<",\"production\":{\"samples\":"<<produce_ns_.size()<<",\"p50Ns\":"<<percentile(50)<<",\"p95Ns\":"<<percentile(95)<<",\"p99Ns\":"<<percentile(99)
                <<",\"maxNs\":"<<(produce_ns_.empty()?0:produce_ns_.back())<<"}}\n";
        }catch(...){}
    }
    host::Viewport viewport()const noexcept override{return viewport_;}
    host::FrameClock::duration frame_interval()const noexcept override{return interval_;}
    bool supports_shared_frames()const noexcept override{return true;}
    bool supports_frame_ready_notifications()const noexcept override{return true;}
    void set_frame_ready_handler(FrameReadyHandler callback)noexcept override{
        std::shared_ptr<ReadyHandler> retired,notify;
        try{
            auto next=callback?std::make_shared<ReadyHandler>(std::move(callback)):nullptr;
            {
                std::lock_guard lock(mutex_);
                retired=std::move(ready_handler_);
                if(retired)retired->active.store(false,std::memory_order_release);
                if(!stopping_)ready_handler_=std::move(next);
                if(!ready_.empty())notify=ready_handler_;
            }
            // Also signal frames that predate registration; never call host
            // code or destroy its former callback while holding our mutex.
            notify_ready(notify);
        }catch(...){fail(std::current_exception());}
    }
    std::shared_ptr<const coastal::rts::view3d::TerrainMesh> terrain_for_metal()override{
        return terrain_;
    }
    void scene_available(bool available)noexcept override{
        enqueue([available](host::Session& s){s.scene_available(available);});
    }
    std::optional<host::Frame> advance_frame()override{return take({});}
    std::optional<host::Frame> advance_frame_into(std::span<std::uint8_t> target)override{return take(target);}
    void pointer_event(host::PointerEvent e)noexcept override{enqueue([e](host::Session& s){s.pointer_event(e);});}
    void scroll_event(host::ScrollEvent e)noexcept override{enqueue([e](host::Session& s){s.scroll_event(e);});}
    void key_event(host::KeyEvent e)noexcept override{
        try{
            const std::string characters(e.characters),plain(e.characters_ignoring_modifiers);
            enqueue([e,characters,plain](host::Session& s)mutable{e.characters=characters;e.characters_ignoring_modifiers=plain;s.key_event(e);});
        }catch(...){fail(std::current_exception());}
    }
    void input_focus_lost()noexcept override{enqueue([](host::Session& s){s.input_focus_lost();});}
private:
    void notify_ready(const std::shared_ptr<ReadyHandler>& handler)noexcept{
        if(!handler||!handler->active.load(std::memory_order_acquire))return;
        try{handler->callback();}catch(...){fail(std::current_exception());}
    }
    void fail(std::exception_ptr error)noexcept{
        std::lock_guard lock(mutex_);error_=error;stopping_=true;condition_.notify_all();
    }
    template<class F>void enqueue(F event)noexcept{
        try{std::lock_guard lock(mutex_);if(!stopping_)inputs_.emplace_back(std::move(event));condition_.notify_all();}
        catch(...){fail(std::current_exception());}
    }
    std::optional<host::Frame> take(std::span<std::uint8_t> target){
        std::shared_ptr<Packet> packet;
        {
            std::lock_guard lock(mutex_);
            if(error_)std::rethrow_exception(error_);
            if(!producer_.joinable())producer_=std::thread([this]{produce();});
            if(ready_.empty()){++empty_polls_;return std::nullopt;}
            packet=std::move(ready_.front());ready_.pop_front();
            ++delivered_;
        }
        condition_.notify_all();
        std::span<const std::uint8_t> pixels=packet->scene?std::span<const std::uint8_t>{}:std::span<const std::uint8_t>{packet->pixels};
        if(!pixels.empty()&&!target.empty()){
            if(target.size()!=pixels.size())throw std::runtime_error("Presentation target differs from produced frame");
            std::copy(pixels.begin(),pixels.end(),target.begin());pixels=target;
            // The host owns and recycles this borrowed target. The packet owns
            // only its source copy, so it cannot promise target immutability.
            return host::Frame{packet->commit,pixels,{},nullptr};
        }
        return host::Frame{packet->commit,pixels,packet,packet->scene?&*packet->scene:nullptr};
    }
    void produce()noexcept{
        try{
            configure_producer_qos();
            auto due=host::FrameClock::now();
            for(;;){
                std::deque<std::function<void(host::Session&)>> inputs;
                {
                    std::unique_lock lock(mutex_);
                    if(ready_.size()>=2)++full_waits_;
                    condition_.wait(lock,[&]{return stopping_||ready_.size()<2;});
                    if(stopping_)return;
                    condition_.wait_until(lock,due,[&]{return stopping_;});
                    if(stopping_)return;
                    inputs.swap(inputs_);
                }
                const auto began=host::FrameClock::now();
                for(auto& event:inputs)event(*session_);
                std::shared_ptr<Packet> packet;
                while(!packet){
                    for(const auto& cached:pool_)if(cached.use_count()==1){packet=cached;break;}
                    if(!packet&&pool_.size()<4){packet=std::make_shared<Packet>();packet->pixels.resize(std::size_t(viewport_.width)*viewport_.height*4);pool_.push_back(packet);}
                    if(!packet){std::unique_lock lock(mutex_);condition_.wait_for(lock,std::chrono::milliseconds(1));if(stopping_)return;}
                }
                auto frame=session_->supports_shared_frames()?session_->advance_frame_into(packet->pixels):session_->advance_frame();
                if(frame){
                    packet->commit=frame->commit;
                    if(frame->scene)packet->scene=*frame->scene;
                    else {packet->scene.reset();if(frame->rgba.data()!=packet->pixels.data())packet->pixels.assign(frame->rgba.begin(),frame->rgba.end());}
                    const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(host::FrameClock::now()-began).count();
                    if(produce_ns_.size()<65536)produce_ns_.push_back(elapsed);
                    ++produced_;
                    std::shared_ptr<ReadyHandler> notify;
                    {
                        std::lock_guard lock(mutex_);if(stopping_)return;
                        ready_.push_back(std::move(packet));peak_ready_=std::max(peak_ready_,ready_.size());
                        notify=ready_handler_;
                    }
                    notify_ready(notify);
                }
                // Never skip a simulation frame or fabricate a repeated commit.
                // A slow producer is visible as a real missing presentation.
                due+=interval_;if(due<host::FrameClock::now()-interval_)due=host::FrameClock::now();
            }
        }catch(...){fail(std::current_exception());}
    }
    std::unique_ptr<host::Session> session_;
    host::Viewport viewport_;
    host::FrameClock::duration interval_;
    const std::shared_ptr<const coastal::rts::view3d::TerrainMesh> terrain_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::function<void(host::Session&)>> inputs_;
    std::deque<std::shared_ptr<Packet>> ready_;
    std::vector<std::shared_ptr<Packet>> pool_;
    std::vector<std::uint64_t> produce_ns_;
    std::uint64_t produced_=0,delivered_=0,empty_polls_=0,full_waits_=0;
    std::size_t peak_ready_=0;
    std::thread producer_;
    std::exception_ptr error_;
    std::shared_ptr<ReadyHandler> ready_handler_;
    bool stopping_=false;
};
}
std::unique_ptr<host::Session> make_presented_session(std::unique_ptr<host::Session> session){
    return session?std::make_unique<PresentedSession>(std::move(session)):nullptr;
}
}
