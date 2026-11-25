// SPDX-License-Identifier: GPL-3.0-only
#include "macos_host/presented_session.h"
#include "rts_3d/scene.h"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <thread>
using namespace std::chrono_literals;
namespace host=coastal::macos_host;
namespace {
void require(bool okay,const char* reason){if(!okay)throw std::runtime_error(reason);}
struct Control{
    std::mutex mutex;std::condition_variable changed;
    bool entered=false,released=false,fail_frame=false;
    std::string key;bool scene=false;std::size_t terrain_calls=0;
    std::thread::id terrain_thread,frame_thread;
    std::thread::id clock_thread;
    std::size_t clock_samples=0,clock_starts=0;
    std::chrono::nanoseconds elapsed{}, last_elapsed{};
    bool wait_for_key=false;
};
struct ReleaseOnExit{
    std::shared_ptr<Control> state;
    ~ReleaseOnExit(){std::lock_guard lock(state->mutex);state->released=true;state->changed.notify_all();}
};
class Fake final:public host::Session{
public:
    explicit Fake(std::shared_ptr<Control> state, host::FrameClock::duration interval=3ms)
        : state_(std::move(state)), interval_(interval) {}
    host::Viewport viewport()const noexcept override{return {2,2};}
    host::FrameClock::duration frame_interval()const noexcept override{return interval_;}
    void set_frame_elapsed(std::chrono::nanoseconds value) noexcept override {
        std::lock_guard lock(state_->mutex);
        state_->clock_thread = std::this_thread::get_id();
        ++state_->clock_samples;
        if (value == 0ns) ++state_->clock_starts;
        state_->elapsed += value;
        state_->last_elapsed = value;
    }
    std::shared_ptr<const coastal::rts::view3d::TerrainMesh> terrain_for_metal()override{
        std::lock_guard lock(state_->mutex);++state_->terrain_calls;state_->terrain_thread=std::this_thread::get_id();return terrain_;
    }
    void pointer_event(host::PointerEvent)noexcept override{}
    void scene_available(bool ready)noexcept override{std::lock_guard lock(state_->mutex);state_->scene=ready;}
    void key_event(host::KeyEvent event)noexcept override{std::lock_guard lock(state_->mutex);state_->key=event.characters;}
    std::optional<host::Frame> advance_frame()override{
        {
            std::unique_lock lock(state_->mutex);
            state_->entered=true;state_->frame_thread=std::this_thread::get_id();state_->changed.notify_all();
            state_->changed.wait(lock,[&]{return state_->released;});
            if(state_->fail_frame)throw std::runtime_error("synthetic frame failure");
            if(state_->wait_for_key && state_->key.empty())return std::nullopt;
        }
        const auto index=frames_++;pixels_.assign(16,std::uint8_t(index+1));scene_.step=index;
        return host::Frame{{index,1200,true},index%2?std::span<const std::uint8_t>{}:std::span<const std::uint8_t>{pixels_},{},index%2?&scene_:nullptr};
    }
private:std::shared_ptr<Control> state_;host::FrameClock::duration interval_;std::shared_ptr<const coastal::rts::view3d::TerrainMesh> terrain_=std::make_shared<coastal::rts::view3d::TerrainMesh>();std::vector<std::uint8_t> pixels_;coastal::rts::view3d::SceneFrame scene_;std::uint64_t frames_=0;
};
host::Frame wait_frame(host::Session& s,std::span<std::uint8_t> target={}){
    const auto end=host::FrameClock::now()+2s;
    while(host::FrameClock::now()<end){if(auto f=target.empty()?s.advance_frame():s.advance_frame_into(target))return *f;std::this_thread::sleep_for(1ms);}
    throw std::runtime_error("producer did not publish a frame");
}
void immutable_frames_and_inputs(){
    auto state=std::make_shared<Control>();auto session=host::make_presented_session(std::make_unique<Fake>(state));
    ReleaseOnExit release{state};
    const auto terrain=session->terrain_for_metal();require(bool(terrain),"setup terrain missing");
    session->scene_available(true);
    {std::string text="borrowed-key";session->key_event({host::KeyEvent::Phase::Down,9,0,false,text,text});}
    require(!session->advance_frame(),"a blocked producer returned a fabricated frame");
    {std::unique_lock lock(state->mutex);require(state->changed.wait_for(lock,2s,[&]{return state->entered;}),"producer never started");}
    require(session->terrain_for_metal()==terrain&&session->terrain_for_metal()==terrain,"later terrain query changed the setup snapshot");
    const auto before=host::FrameClock::now();require(!session->advance_frame(),"blocked producer unexpectedly became ready");
    require(host::FrameClock::now()-before<20ms,"GUI waited for simulation work");
    {std::lock_guard lock(state->mutex);require(state->scene&&state->key=="borrowed-key","setup/input order or borrowed key lifetime changed");require(state->terrain_calls==1&&state->terrain_thread==std::this_thread::get_id()&&state->frame_thread!=std::this_thread::get_id(),"terrain query reached the worker-owned runtime after setup");state->released=true;state->changed.notify_all();}
    const auto first=wait_frame(*session);require(first.commit.index==0&&first.rgba_owner&&first.rgba.front()==1,"first immutable frame invalid");
    const auto second=wait_frame(*session);require(second.commit.index==1&&second.scene&&second.scene->step==1&&second.rgba_owner,"owned 3D scene missing");
    const auto third=wait_frame(*session);require(third.commit.index==2&&third.rgba.front()==3,"producer dropped or repeated a commit");
    require(first.rgba.front()==1&&second.scene->step==1,"new production mutated a pending presentation");
    session.reset();require(first.rgba.front()==1&&second.scene->step==1,"shutdown invalidated retained frame storage");
}
void borrowed_target_frames(){
    auto state=std::make_shared<Control>();state->released=true;
    auto session=host::make_presented_session(std::make_unique<Fake>(state));
    std::array<std::uint8_t,16> target{};
    const auto first=wait_frame(*session,target);
    require(first.commit.index==0&&first.rgba.data()==target.data()&&first.rgba.size()==target.size()&&first.rgba.front()==1&&!first.rgba_owner&&!first.scene,"copied host target advertises a false immutable owner");
    const auto second=wait_frame(*session,target);
    require(second.commit.index==1&&second.rgba.empty()&&second.rgba_owner&&second.scene&&second.scene->step==1,"target path failed to retain an immutable scene");
    const auto third=wait_frame(*session,target);
    require(third.commit.index==2&&third.rgba.data()==target.data()&&third.rgba.front()==3&&!third.rgba_owner,"host target was not reused as borrowed storage");
    session.reset();require(second.scene->step==1,"target-path scene lifetime ended at shutdown");
}
void producer_error(){
    auto state=std::make_shared<Control>();state->released=true;state->fail_frame=true;
    auto session=host::make_presented_session(std::make_unique<Fake>(state));
    bool forwarded=false;
    try{(void)wait_frame(*session);}catch(const std::runtime_error& error){forwarded=std::string(error.what())=="synthetic frame failure";}
    require(forwarded,"producer exception was lost or escaped its thread");
}
void elapsed_time_belongs_to_the_producer() {
    auto state=std::make_shared<Control>();
    state->released=true;
    auto session=host::make_presented_session(std::make_unique<Fake>(state));
    wait_frame(*session);
    wait_frame(*session);
    {
        std::lock_guard lock(state->mutex);
        require(state->clock_samples>=2 && state->clock_starts==1 && state->elapsed>0ns,
                "A running producer must initialize once and receive physical elapsed time");
        require(state->clock_thread==state->frame_thread && state->clock_thread!=std::this_thread::get_id(),
                "The simulation owner must sample its own clock");
    }
    session->reset_frame_elapsed();
    // Consume any pictures already published before the main thread resumed.
    const auto deadline=host::FrameClock::now()+2s;
    while(host::FrameClock::now()<deadline) {
        wait_frame(*session);
        std::lock_guard lock(state->mutex);
        if(state->clock_starts==2)return;
    }
    throw std::runtime_error("Resuming did not reset the clock on the simulation owner");
}
void wall_clock_backpressure() {
    const host::LaunchOptions defaults;
    require(defaults.producer_pacing == host::ProducerPacing::RuntimeInterval, "Default pacing changed");
    auto state = std::make_shared<Control>();
    state->released = true;
    const auto session = host::make_presented_session(std::make_unique<Fake>(state, 5s),
                                                      host::ProducerPacing::WallClockBackpressure);
    // Nominal cadence is deliberately much longer than the test deadline.
    (void)session->advance_frame();
    {
        std::unique_lock lock(state->mutex);
        require(state->changed.wait_for(lock, 1s, [&]{return state->clock_samples == 2;}),
                "Producer did not fill both FIFO slots");
        require(!state->changed.wait_for(lock, 30ms, [&]{return state->clock_samples > 2;}),
                "A full FIFO advanced the simulation");
    }
    require(wait_frame(*session).commit.index == 0, "First queued commit was replaced");
    {
        std::unique_lock lock(state->mutex);
        require(state->changed.wait_for(lock, 1s, [&]{return state->clock_samples == 3;}),
                "Consumer release did not unblock the producer");
        require(state->last_elapsed >= 25ms, "Elapsed time omitted the wait for FIFO space");
    }
    require(wait_frame(*session).commit.index == 1, "Second queued commit was skipped");
    require(wait_frame(*session).commit.index == 2, "Released FIFO lost its next commit");
}
void empty_wall_clock_session_waits_for_input() {
    auto state = std::make_shared<Control>();
    state->released = true;
    state->wait_for_key = true;
    auto session = host::make_presented_session(std::make_unique<Fake>(state, 5s),
                                                host::ProducerPacing::WallClockBackpressure);
    (void)session->advance_frame();
    {
        std::unique_lock lock(state->mutex);
        require(state->changed.wait_for(lock, 1s, [&]{return state->entered;}), "No first attempt");
        require(!state->changed.wait_for(lock, 30ms, [&]{return state->clock_samples > 1;}),
                "Empty session retried continuously");
    }
    session->key_event({host::KeyEvent::Phase::Down, 1, 0, false, "go", "go"});
    require(wait_frame(*session).commit.index == 0, "Input failed to wake idle producer");
}
void revoked_handler_does_not_receive_a_later_publication(){
    auto state=std::make_shared<Control>();
    auto inner=std::make_unique<Fake>(state);
    require(!inner->supports_frame_ready_notifications(),"a synchronous session opted into notifications by default");
    auto session=host::make_presented_session(std::move(inner));
    ReleaseOnExit release{state};
    require(session->supports_frame_ready_notifications(),"the publisher does not advertise ready notifications");
    const auto calls=std::make_shared<std::atomic<unsigned>>(0);
    session->set_frame_ready_handler([calls]{++*calls;});
    require(!session->advance_frame(),"a blocked publisher fabricated a ready frame");
    {std::unique_lock lock(state->mutex);require(state->changed.wait_for(lock,2s,[&]{return state->entered;}),"publisher never entered its frame");}
    session->set_frame_ready_handler({});
    {std::lock_guard lock(state->mutex);state->released=true;state->changed.notify_all();}
    const auto frame=wait_frame(*session);
    require(frame.commit.index==0,"revoking notifications lost the actual frame");
    session.reset();
    require(calls->load()==0,"a revoked registration received a later publication");
}
struct HostInbox{
    std::mutex mutex;std::condition_variable changed;
    std::vector<std::function<void()>> queued;
    std::thread::id publisher;
    std::atomic<bool> active{true};
};
void queued_ready_notifications_do_not_retain_a_closed_host(){
    auto state=std::make_shared<Control>();
    auto session=host::make_presented_session(std::make_unique<Fake>(state));
    ReleaseOnExit release{state};
    auto inbox=std::make_shared<HostInbox>();
    auto applied=std::make_shared<std::atomic<unsigned>>(0);
    const auto weak=std::weak_ptr<HostInbox>(inbox);
    session->set_frame_ready_handler([weak,applied]{
        if(const auto host=weak.lock()){
            {std::lock_guard lock(host->mutex);host->publisher=std::this_thread::get_id();
                host->queued.push_back([weak,applied]{if(const auto live=weak.lock();live&&live->active.load())++*applied;});}
            host->changed.notify_all();
        }
    });
    require(!session->advance_frame(),"the notifier fabricated an initial frame");
    {std::lock_guard lock(state->mutex);state->released=true;state->changed.notify_all();}
    std::function<void()> live_notification;
    {std::unique_lock lock(inbox->mutex);require(inbox->changed.wait_for(lock,2s,[&]{return inbox->queued.size()>=2;}),"publications did not notify the host");
        require(inbox->publisher!=std::this_thread::get_id(),"ready notification did not run on its publisher");
        live_notification=std::move(inbox->queued.front());inbox->queued.erase(inbox->queued.begin());}
    live_notification();
    require(applied->load()==1,"a live publisher notification was not delivered");
    inbox->active.store(false);
    session->set_frame_ready_handler({});
    // The host still has an unexecuted notification. Joining must not
    // require main-queue work or keep the closed callback target alive.
    session.reset();
    std::vector<std::function<void()>> delayed;
    {std::lock_guard lock(inbox->mutex);delayed.swap(inbox->queued);}
    inbox.reset();
    require(weak.expired(),"queued notification retained the closed host");
    for(auto& notification:delayed)notification();
    require(applied->load()==1,"a late publisher callback reached the closed host");
}
class BrokenOutput final:public std::streambuf{
    int_type overflow(int_type)override{throw std::runtime_error("synthetic diagnostic failure");}
    std::streamsize xsputn(const char*,std::streamsize)override{throw std::runtime_error("synthetic diagnostic failure");}
};
void diagnostic_failure(){
    auto session=host::make_presented_session(std::make_unique<Fake>(std::make_shared<Control>()));
    BrokenOutput broken;
    const auto mask=std::cout.exceptions();auto* original=std::cout.rdbuf(&broken);
    std::cout.exceptions(std::ios::badbit|std::ios::failbit);
    session.reset();
    std::cout.exceptions(std::ios::goodbit);std::cout.clear();std::cout.rdbuf(original);std::cout.exceptions(mask);
}
}
int main(){try{
    immutable_frames_and_inputs();borrowed_target_frames();producer_error();
    elapsed_time_belongs_to_the_producer();wall_clock_backpressure();empty_wall_clock_session_waits_for_input();
    revoked_handler_does_not_receive_a_later_publication();queued_ready_notifications_do_not_retain_a_closed_host();diagnostic_failure();
    std::cout<<"MAC_HOST_PRESENTED_SESSION_PASS nonblocking=true immutablePixels=true immutableScene=true inputsOwned=true sequential=true cleanJoin=true borrowedTarget=true cachedTerrain=true forwardedError=true diagnosticNoThrow=true wallFifo=true emptyWait=true inputWake=true readyNotification=true handlerRevoked=true lateHostCallbackSafe=true\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
