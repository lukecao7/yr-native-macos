// SPDX-License-Identifier: GPL-3.0-only
// Synthetic blocking-dispatch benchmark. No game data or scheduling setters.
#include "rts_core/worker_pool.h"
#include "profiling/thread_observation.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#if defined(__APPLE__) || defined(__linux__)
#include <sys/utsname.h>
#endif

namespace {
using coastal::rts::WorkerPool;
using Clock = std::chrono::steady_clock;
constexpr unsigned warmups = 100;

std::uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

struct alignas(128) Lane {
    std::uint64_t began = 0, ended = 0, checksum = 0;
};

struct Summary {
    std::uint64_t mean = 0, p50 = 0, p95 = 0;
};

Summary summarize(std::vector<std::uint64_t> values) {
    const auto sum = std::accumulate(values.begin(), values.end(), std::uint64_t{});
    std::sort(values.begin(), values.end());
    return {sum / values.size(), values[(values.size()*50+99)/100-1], values[(values.size()*95+99)/100-1]};
}

void print_summary(const char* name, Summary value) {
    std::cout << ",\"" << name << "\":{\"mean\":" << value.mean
              << ",\"p50\":" << value.p50 << ",\"p95\":" << value.p95 << '}';
}

void print_identity(coastal::profiling::ThreadIdentity value) {
    std::cout << "{\"supported\":" << (value.supported ? "true" : "false")
              << ",\"threadId\":" << value.thread_id << ",\"threadIdReadResult\":" << value.thread_id_status
              << ",\"requestedClass\":" << value.requested_qos << ",\"relativePriority\":" << value.relative_priority
              << ",\"qosReadResult\":" << value.qos_status << '}';
}

void print_platform() {
    std::string system = "unavailable", release = "unavailable", machine = "unavailable";
    std::string compiler = "unavailable";
#ifdef __VERSION__
    compiler = __VERSION__;
#endif
#if defined(__APPLE__) || defined(__linux__)
    utsname value{};
    if (uname(&value) == 0) {
        system = value.sysname; release = value.release; machine = value.machine;
    }
#endif
    std::cout << ",\"platform\":{\"system\":" << std::quoted(system)
              << ",\"release\":" << std::quoted(release) << ",\"machine\":" << std::quoted(machine)
              << ",\"hardwareThreadsReported\":" << std::thread::hardware_concurrency()
              << ",\"compiler\":" << std::quoted(compiler) << '}';
}

std::uint64_t run(unsigned count, std::size_t items, unsigned rounds, const std::string& label,
                  const std::uint64_t* expected_checksum) {
    WorkerPool pool(count, false);
    std::array<coastal::profiling::ThreadIdentity, 8> before{}, after{};
    pool.run(count, [&](unsigned lane, std::size_t, std::size_t) {
        before[lane] = coastal::profiling::observe_current_thread_identity();
    });
    std::array<Lane, 8> lanes{};
    std::vector<std::uint64_t> wall, entry_spread, return_tail;
    wall.reserve(rounds); entry_spread.reserve(rounds); return_tail.reserve(rounds);
    const auto participants = std::min<std::size_t>(items, count);
    for (unsigned round = 0; round < rounds+warmups; ++round) {
        const auto began = now_ns();
        pool.run(items, [&](unsigned lane, std::size_t begin, std::size_t end) {
            auto& record = lanes[lane];
            record.began = now_ns();
            std::uint64_t value = 0;
            // Fixed useful integer work. Each item's value is independent of
            // partitioning, so the checksum must match the one-lane run.
            for (auto n = begin; n < end; ++n) {
                auto mixed = std::uint64_t(n+1) ^ ((std::uint64_t(round)+1)*0x9e3779b97f4a7c15ULL);
                mixed = (mixed ^ (mixed >> 30))*0xbf58476d1ce4e5b9ULL;
                mixed = (mixed ^ (mixed >> 27))*0x94d049bb133111ebULL;
                value += mixed ^ (mixed >> 31);
            }
            record.checksum += value;
            record.ended = now_ns();
        });
        const auto ended = now_ns();
        if (round < warmups) continue;
        auto earliest = lanes[0].began, latest = earliest, finished = lanes[0].ended;
        for (std::size_t lane = 1; lane < participants; ++lane) {
            earliest = std::min(earliest, lanes[lane].began);
            latest = std::max(latest, lanes[lane].began);
            finished = std::max(finished, lanes[lane].ended);
        }
        wall.push_back(ended-began);
        entry_spread.push_back(latest-earliest);
        return_tail.push_back(ended-finished);
    }
    pool.run(count, [&](unsigned lane, std::size_t, std::size_t) {
        after[lane] = coastal::profiling::observe_current_thread_identity();
    });
    std::uint64_t checksum = 0;
    for (const auto& lane : lanes) checksum += lane.checksum;
    if (expected_checksum && checksum != *expected_checksum) {
        throw std::runtime_error("Dispatched useful-work checksum differs from one lane");
    }
    std::cout << "WORKER_POOL_DISPATCH_BENCH {\"label\":" << std::quoted(label)
              << ",\"configuredLanes\":" << count << ",\"activeLanes\":" << participants
              << ",\"items\":" << items << ",\"rounds\":" << rounds << ",\"warmups\":" << warmups
              << ",\"workerUsageInstrumentation\":false,\"osSchedulingPolicyChanged\":false"
              << ",\"callbackClockReadsIncluded\":true,\"checksumIncludesWarmups\":true,\"checksum\":" << checksum;
    print_platform();
    print_summary("wallNs", summarize(wall));
    print_summary("entrySpreadNs", summarize(entry_spread));
    print_summary("returnAfterLastCallbackNs", summarize(return_tail));
    std::cout << ",\"observedThreadEndpoints\":[";
    for (unsigned lane = 0; lane < count; ++lane) {
        if (lane) std::cout << ',';
        std::cout << "{\"lane\":" << lane << ",\"before\":";
        print_identity(before[lane]);
        std::cout << ",\"after\":";
        print_identity(after[lane]);
        std::cout << '}';
    }
    std::cout << "]}\n";
    return checksum;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        unsigned rounds = 2000;
        std::string label = "unspecified";
        for (int n = 1; n < argc; ++n) {
            const std::string_view argument = argv[n];
            if (argument == "--help") {
                std::cout << "Usage: worker_pool_dispatch_bench [--rounds=2000] [--label=build-name]\n";
                return 0;
            }
            if (argument.starts_with("--rounds=")) {
                const std::string text(argument.substr(9));
                std::size_t consumed = 0;
                const auto value = std::stoul(text, &consumed);
                if (consumed != text.size() || value < 100 || value > 100000) {
                    throw std::invalid_argument("rounds must be 100..100000");
                }
                rounds = static_cast<unsigned>(value);
            } else if (argument.starts_with("--label=")) {
                label = argument.substr(8);
                if (label.empty() || label.size() > 64 || label.find_first_not_of(
                        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") != std::string::npos) {
                    throw std::invalid_argument("label must contain 1..64 letters, digits, hyphens, underscores or dots");
                }
            } else {
                throw std::invalid_argument("unknown argument: " + std::string(argument));
            }
        }
        constexpr std::array<std::size_t, 4> work_items{2, 8, 1200, 12000};
        std::array<std::uint64_t, work_items.size()> checksums{};
        for (const unsigned lanes : {1U, 4U, 8U}) {
            for (std::size_t n = 0; n < work_items.size(); ++n) {
                const auto value = run(lanes, work_items[n], rounds, label, lanes == 1 ? nullptr : &checksums[n]);
                if (lanes == 1) checksums[n] = value;
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
