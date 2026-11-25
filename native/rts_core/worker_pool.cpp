// SPDX-License-Identifier: GPL-3.0-only
#include "worker_pool.h"

#include "profiling/thread_observation.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace coastal::rts {

struct WorkerPool::Impl {
    static constexpr unsigned max_count = 8;
    // A broadcast made every awakened lane queue on the same ready mutex.
    // Independent sleeping mailboxes keep publication local to each lane;
    // pending work remains visible even before that lane reaches its wait.
    struct alignas(128) Wakeup {
        std::mutex mutex;
        std::condition_variable ready;
        const Job* task = nullptr;
        std::size_t items = 0;
        unsigned lanes = 0;
        bool stopping = false;
    };

    Impl(unsigned count, bool measure) : count(count), measure(measure), usage(count) {
        threads.reserve(count - 1);
        try {
            for (unsigned lane = 1; lane < count; ++lane) {
                threads.emplace_back([this, lane] { wait_for_work(lane); });
            }
        } catch (...) {
            stop();
            throw;
        }
    }

    ~Impl() { stop(); }

    void stop() {
        for (unsigned lane = 1; lane < count; ++lane) {
            auto& wakeup = wakeups[lane];
            {
                std::lock_guard lock(wakeup.mutex);
                wakeup.stopping = true;
            }
            wakeup.ready.notify_one();
        }
        for (auto& thread : threads) thread.join();
    }

    void execute(unsigned lane, std::size_t items, unsigned lanes, const Job& task) noexcept {
        const std::size_t base = items / lanes;
        const std::size_t extra = items % lanes;
        const std::size_t begin = base * lane + std::min<std::size_t>(lane, extra);
        const std::size_t end = begin + base + (lane < extra);
        auto& measurement = usage[lane];
        profiling::ThreadCpuSample start;
        if (measure) {
            if (!measurement.jobs) {
                measurement.thread_id = profiling::observe_current_thread_identity().thread_id;
            }
            start = profiling::observe_current_thread_cpu();
        }
        try {
            task(lane, begin, end);
        } catch (...) {
            failures[lane] = std::current_exception();
        }
        if (measure) {
            const auto finish = profiling::observe_current_thread_cpu();
            if (start.valid && finish.valid && finish.ns >= start.ns) {
                measurement.cpu_ns += finish.ns - start.ns;
            }
            ++measurement.jobs;
        }
    }

    void wait_for_work(unsigned lane) {
        auto& wakeup = wakeups[lane];
        for (;;) {
            std::unique_lock lock(wakeup.mutex);
            wakeup.ready.wait(lock, [&] { return wakeup.stopping || wakeup.task; });
            if (wakeup.stopping) return;
            const auto* task = wakeup.task;
            const auto items = wakeup.items;
            const auto lanes = wakeup.lanes;
            wakeup.task = nullptr;
            lock.unlock();
            execute(lane, items, lanes, *task);
            // All execute writes, including exceptions and observations,
            // remain published by the original blocking completion join.
            std::lock_guard completed(mutex);
            if (!--remaining) joined.notify_one();
        }
    }

    void run(std::size_t items, const Job& task, bool shared_queue = false) {
        if (!items) return;
        if (!task) throw std::invalid_argument("Worker pool requires a job.");
        if (dispatching.test_and_set(std::memory_order_acquire)) {
            throw std::logic_error("A worker pool dispatch is already running.");
        }
        struct Release {
            std::atomic_flag& flag;
            ~Release() { flag.clear(std::memory_order_release); }
        } release{dispatching};
        failures.fill(nullptr);

        const auto lanes = static_cast<unsigned>(std::min<std::size_t>(items, count));
        if (lanes == 1) {
            execute(0, items, lanes, task);
        } else {
            {
                std::lock_guard lock(mutex);
                remaining = lanes - 1;
            }
            for (unsigned lane = 1; lane < lanes; ++lane) {
                auto& wakeup = wakeups[lane];
                {
                    std::lock_guard lock(wakeup.mutex);
                    wakeup.items = items;
                    wakeup.lanes = lanes;
                    wakeup.task = &task;
                }
                wakeup.ready.notify_one();
            }
            execute(0, items, lanes, task);
            std::unique_lock lock(mutex);
            if (shared_queue && !failures[0]) {
                // Lane zero exhausted the caller's shared claim counter.
                // A worker releases its mailbox before it can join here,
                // so holding the completion lock cannot block its execute.
                for (unsigned lane = 1; lane < lanes && remaining; ++lane) {
                    auto& wakeup = wakeups[lane];
                    std::lock_guard pending(wakeup.mutex);
                    if (wakeup.task == &task) {
                        wakeup.task = nullptr;
                        --remaining;
                    }
                }
            }
            joined.wait(lock, [&] { return !remaining; });
        }
        for (unsigned lane = 0; lane < lanes; ++lane) {
            if (failures[lane]) std::rethrow_exception(failures[lane]);
        }
    }

    const unsigned count;
    const bool measure;
    std::vector<WorkerUsage> usage;
    std::array<std::exception_ptr, max_count> failures{};
    std::vector<std::thread> threads;
    std::array<Wakeup, max_count> wakeups;
    std::mutex mutex;
    std::condition_variable joined;
    std::atomic_flag dispatching{};
    unsigned remaining = 0;
};

WorkerPool::WorkerPool(unsigned count, bool measure) {
    if (!count || count > Impl::max_count) {
        throw std::invalid_argument("Worker pool requires between 1 and 8 lanes.");
    }
    impl_ = std::make_unique<Impl>(count, measure);
}

WorkerPool::~WorkerPool() = default;
WorkerPool::WorkerPool(WorkerPool&&) noexcept = default;
WorkerPool& WorkerPool::operator=(WorkerPool&&) noexcept = default;

unsigned WorkerPool::count() const noexcept { return impl_->count; }
void WorkerPool::run(std::size_t items, const Job& job) { impl_->run(items, job); }
void WorkerPool::run_shared_queue(std::size_t lane_hint, const Job& drain) { impl_->run(lane_hint, drain, true); }
const std::vector<WorkerUsage>& WorkerPool::usage() const noexcept { return impl_->usage; }

}  // namespace coastal::rts
