// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace coastal::rts {

struct WorkerUsage {
    std::uint64_t jobs = 0;
    std::uint64_t cpu_ns = 0;
    std::uint64_t thread_id = 0;
};

// Owner thread dispatches nonoverlapping ranges and waits for every range.
// Lane zero is the caller. All other lanes are persistent worker threads.
class WorkerPool {
public:
    using Job = std::function<void(unsigned lane, std::size_t begin, std::size_t end)>;

    explicit WorkerPool(unsigned count = 1, bool measure = false);
    ~WorkerPool();
    WorkerPool(WorkerPool&&) noexcept;
    WorkerPool& operator=(WorkerPool&&) noexcept;
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    unsigned count() const noexcept;
    void run(std::size_t items, const Job& job);
    const std::vector<WorkerUsage>& usage() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace coastal::rts
