// SPDX-License-Identifier: GPL-3.0-only
#include "rts_core/combat.h"
#include "rts_core/snapshot.h"
#include "rts_core/stress.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using namespace coastal::rts;
using Clock = std::chrono::steady_clock;

constexpr std::uint64_t hash_seed = 14695981039346656037ULL;
constexpr unsigned expected_units = 1'200;
constexpr unsigned minimum_active = 600;
constexpr unsigned checkpoint_every = 60;

struct Options {
    std::vector<unsigned> threads{1, 4, 8};
    unsigned steps = 3'600;
};

unsigned read_number(std::string_view text) {
    unsigned value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || !value) {
        throw std::invalid_argument("Expected a positive integer.");
    }
    return value;
}

std::vector<unsigned> read_threads(std::string_view text) {
    std::vector<unsigned> counts;
    for (;;) {
        const auto separator = text.find(',');
        const auto count = read_number(text.substr(0, separator));
        if ((count != 1 && count != 4 && count != 8) ||
            std::find(counts.begin(), counts.end(), count) != counts.end()) {
            throw std::invalid_argument("Thread counts must be distinct values from 1, 4 and 8.");
        }
        counts.push_back(count);
        if (separator == std::string_view::npos) break;
        text.remove_prefix(separator + 1);
    }
    return counts;
}

Options options(int argc, char** argv) {
    Options result;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg.starts_with("--threads=")) {
            result.threads = read_threads(arg.substr(std::string_view("--threads=").size()));
        } else if (arg.starts_with("--steps=")) {
            result.steps = read_number(arg.substr(std::string_view("--steps=").size()));
        } else {
            throw std::invalid_argument("Use --threads=1,4,8 and --steps=3600.");
        }
    }
    return result;
}

std::uint64_t hash_bytes(std::span<const std::uint8_t> bytes, std::uint64_t hash = hash_seed) {
    for (const auto byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string hex_hash(std::uint64_t hash) {
    std::ostringstream text;
    text << std::hex << std::setfill('0') << std::setw(16) << hash;
    return text.str();
}

struct Checkpoint {
    std::uint64_t step;
    std::uint64_t world_hash;
    std::uint64_t vision_hash;
    std::size_t snapshot_bytes;

    bool operator==(const Checkpoint&) const = default;
};

struct Result {
    unsigned threads;
    unsigned steps;
    std::uint64_t simulation_ns = 0;
    unsigned min_units = std::numeric_limits<unsigned>::max();
    unsigned min_active = std::numeric_limits<unsigned>::max();
    unsigned min_moved = std::numeric_limits<unsigned>::max();
    unsigned min_engaged = std::numeric_limits<unsigned>::max();
    unsigned firing_steps = 0;
    std::uint64_t total_fired = 0;
    std::uint64_t total_moves = 0;
    std::size_t flow_peak_bytes = 0;
    std::vector<WorkerUsage> workers;
    std::vector<Checkpoint> checkpoints;

    double average_ns() const { return static_cast<double>(simulation_ns) / steps; }
};

Result run(unsigned threads, unsigned steps) {
    auto world = make_stress_match();
    Navigator navigator(threads, true);
    Result result{.threads = threads, .steps = steps, .workers = {}, .checkpoints = {}};

    for (unsigned i = 0; i < steps; ++i) {
        const auto start = Clock::now();
        drive_stress(world);
        world.tick(navigator);
        result.simulation_ns +=
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();

        const auto& work = navigator.last_work();
        result.min_units = std::min(result.min_units, static_cast<unsigned>(world.entities.size()));
        result.min_active = std::min(result.min_active, work.active);
        result.min_moved = std::min(result.min_moved, work.moved);
        result.min_engaged = std::min(result.min_engaged, work.engaged);
        result.total_moves += work.moved;
        result.total_fired += work.fired;
        result.firing_steps += work.fired > 0;

        if (world.step % checkpoint_every == 0 || world.step == steps) {
            const auto bytes = encode_snapshot(world);
            const auto blue_vision = hash_bytes(navigator.visibility(Side::Blue));
            const auto vision = hash_bytes(navigator.visibility(Side::Copper), blue_vision);
            result.checkpoints.push_back({world.step, hash_bytes(bytes), vision, bytes.size()});
        }
    }
    result.flow_peak_bytes = navigator.flow_stats().peak_total_distance_bytes;
    result.workers = navigator.workers().usage();
    return result;
}

void print(const Result& result) {
    std::set<std::uint64_t> measured_workers;
    for (std::size_t i = 1; i < result.workers.size(); ++i) {
        const auto& worker = result.workers[i];
        if (worker.thread_id && worker.cpu_ns && worker.jobs) measured_workers.insert(worker.thread_id);
    }

    std::cout << "RTS_STRESS_RUN {\"threads\":" << result.threads
              << ",\"steps\":" << result.steps
              << ",\"averageTickNs\":" << result.average_ns()
              << ",\"minUnits\":" << result.min_units
              << ",\"minActive\":" << result.min_active
              << ",\"minMoved\":" << result.min_moved
              << ",\"minEngaged\":" << result.min_engaged
              << ",\"firingSteps\":" << result.firing_steps
              << ",\"totalFired\":" << result.total_fired
              << ",\"totalMoves\":" << result.total_moves
              << ",\"flowPeakBytes\":" << result.flow_peak_bytes
              << ",\"nativeWorkerThreads\":" << measured_workers.size()
              << ",\"workerUsage\":[";
    bool comma = false;
    for (const auto& worker : result.workers) {
        if (comma) std::cout << ',';
        comma = true;
        std::cout << "{\"id\":" << worker.thread_id
                  << ",\"cpuNs\":" << worker.cpu_ns << ",\"jobs\":" << worker.jobs << '}';
    }
    std::cout << "],\"checkpoints\":[";
    comma = false;
    for (const auto& checkpoint : result.checkpoints) {
        if (comma) std::cout << ',';
        comma = true;
        std::cout << "{\"step\":" << checkpoint.step
                  << ",\"snapshotBytes\":" << checkpoint.snapshot_bytes
                  << ",\"worldHash\":\"" << hex_hash(checkpoint.world_hash)
                  << "\",\"visionHash\":\"" << hex_hash(checkpoint.vision_hash) << "\"}";
    }
    std::cout << "]}" << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto settings = options(argc, argv);
        std::vector<Result> results;
        bool workload_ok = true;
        for (const auto threads : settings.threads) {
            results.push_back(run(threads, settings.steps));
            const auto& result = results.back();
            print(result);
            workload_ok &= result.min_units == expected_units && result.min_active >= minimum_active &&
                           result.min_moved > 0 && result.firing_steps == settings.steps;
        }

        const auto single_lane = std::find_if(results.begin(), results.end(), [](const Result& result) {
            return result.threads == 1;
        });
        const auto& baseline = single_lane == results.end() ? results.front() : *single_lane;
        const bool identical = std::all_of(results.begin(), results.end(), [&](const Result& result) {
            return result.checkpoints == baseline.checkpoints;
        });
        std::cout << "RTS_STRESS_COMPARE {\"identical\":" << std::boolalpha << identical
                  << ",\"workloadOk\":" << workload_ok
                  << ",\"baselineThreads\":" << baseline.threads
                  << ",\"checkpointCount\":" << baseline.checkpoints.size()
                  << ",\"speedupByThreads\":{";
        bool comma = false;
        for (const auto& result : results) {
            if (comma) std::cout << ',';
            comma = true;
            std::cout << '"' << result.threads << "\":"
                      << (result.simulation_ns ? baseline.average_ns() / result.average_ns() : 0.0);
        }
        std::cout << "}}\n";
        return identical && workload_ok ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "RTS stress: " << error.what() << '\n';
        return 1;
    }
}
