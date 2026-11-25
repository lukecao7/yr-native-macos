// SPDX-License-Identifier: GPL-3.0-only
#include "rts_core/combat.h"
#include "rts_core/economy.h"
#include "rts_core/snapshot.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using namespace coastal::rts;

void require(bool condition, const char* message) {
    if (condition) return;
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

void dispatch_and_reuse() {
    for (const unsigned count : {1, 4, 8}) {
        WorkerPool pool(count, true);
        std::array<std::thread::id, 8> lane_ids;
        std::array<unsigned, 43> visits{};
        constexpr unsigned rounds = 12;

        for (unsigned round = 0; round < rounds; ++round) {
            pool.run(visits.size(), [&](unsigned lane, std::size_t begin, std::size_t end) {
                const auto self = std::this_thread::get_id();
                if (!round) lane_ids[lane] = self;
                require(lane_ids[lane] == self, "A lane changed its thread between tasks");
                for (auto i = begin; i < end; ++i) ++visits[i];
            });
        }
        require(std::all_of(visits.begin(), visits.end(), [](unsigned hits) { return hits == rounds; }),
                "Dispatched ranges must exactly cover all items without duplication");
        require(lane_ids.front() == std::this_thread::get_id(), "The owner runs lane zero");
        for (unsigned lane = 0; lane < count; ++lane) {
            require(pool.usage()[lane].jobs == rounds, "Job accounting must cover each lane");
            for (unsigned previous = 0; previous < lane; ++previous) {
                require(lane_ids[previous] != lane_ids[lane], "A lane must have its own persistent thread");
            }
#ifdef __APPLE__
            require(pool.usage()[lane].thread_id > 0 && pool.usage()[lane].cpu_ns > 0,
                    "Native observations must include real thread identity and CPU time");
#endif
        }

        pool.run(1, [&](unsigned lane, std::size_t begin, std::size_t end) {
            require(lane == 0 && begin == 0 && end == 1, "A single item uses the owner");
        });
        pool.run(0, [](unsigned, std::size_t, std::size_t) {
            require(false, "An empty dispatch must call no lane");
        });
    }
}

void join_on_error() {
    WorkerPool pool(4);
    std::array<unsigned, 4> visits{};
    bool thrown = false;
    try {
        pool.run(visits.size(), [&](unsigned lane, std::size_t, std::size_t) {
            ++visits[lane];
            if (lane == 2) throw std::runtime_error("lane failure");
        });
    } catch (const std::runtime_error& error) {
        thrown = std::string(error.what()) == "lane failure";
    }
    require(thrown, "A lane failure must reach the owner");
    require((visits == std::array<unsigned, 4>{1, 1, 1, 1}), "Errors must still join every lane");
    pool.run(visits.size(), [&](unsigned lane, std::size_t, std::size_t) { ++visits[lane]; });
    require((visits == std::array<unsigned, 4>{2, 2, 2, 2}), "The pool must be reusable after an error");
}

void alternating_participants_and_errors() {
    WorkerPool pool(8);
    for (unsigned round = 0; round < 32; ++round) {
        for (const std::size_t items : {0U, 1U, 2U, 7U, 8U, 43U, 3U, 1U, 8U}) {
            std::array<unsigned, 8> callbacks{};
            std::array<unsigned, 43> visits{};
            pool.run(items, [&](unsigned lane, std::size_t begin, std::size_t end) {
                require(lane < std::min<std::size_t>(8, items), "An inactive lane received another dispatch");
                require(begin < end && end <= items, "A lane retained a previous dispatch range");
                ++callbacks[lane];
                for (auto n = begin; n < end; ++n) ++visits[n];
            });
            for (std::size_t lane = 0; lane < callbacks.size(); ++lane) {
                require(callbacks[lane] == unsigned(lane < std::min<std::size_t>(8, items)),
                        "A participating lane missed or repeated a task");
            }
            for (std::size_t n = 0; n < visits.size(); ++n) {
                require(visits[n] == unsigned(n < items), "Alternating dispatch sizes changed exact item coverage");
            }
        }
    }

    std::array<unsigned, 8> completed{};
    bool ordered = false;
    try {
        pool.run(8, [&](unsigned lane, std::size_t, std::size_t) {
            ++completed[lane];
            if (lane == 0) throw std::runtime_error("owner failure");
            if (lane == 1 || lane == 5) throw std::runtime_error("worker failure");
        });
    } catch (const std::runtime_error& error) {
        ordered = std::string(error.what()) == "owner failure";
    }
    require(ordered, "Exception selection must remain in lane order after joining everyone");
    require(std::all_of(completed.begin(), completed.end(), [](auto value) { return value == 1; }),
            "Multiple failures skipped an outstanding lane");
    pool.run(1, [](unsigned lane, std::size_t begin, std::size_t end) {
        require(lane == 0 && begin == 0 && end == 1, "Owner reuse failed after multiple exceptions");
    });
    pool.run(8, [&](unsigned lane, std::size_t, std::size_t) { ++completed[lane]; });
    require(std::all_of(completed.begin(), completed.end(), [](auto value) { return value == 2; }),
            "A stale mailbox survived error recovery");
}

void shared_queue_dispatch() {
    for (unsigned lanes : {1, 4, 8}) {
        WorkerPool pool(lanes);
        for (unsigned round = 0; round < 256; ++round) {
            std::array<std::atomic<unsigned>, 29> visits{};
            std::atomic<std::size_t> next{0};
            pool.run_shared_queue(lanes, [&](unsigned, std::size_t, std::size_t) {
                for (auto i = next.fetch_add(1); i < visits.size(); i = next.fetch_add(1)) {
                    ++visits[i];
                }
            });
            for (const auto& visit : visits) require(visit.load() == 1, "Each shared item must run exactly once");
        }
    }
    WorkerPool pool(2);
    std::latch started(2);
    bool helper_completed = false;
    try {
        pool.run_shared_queue(2, [&](unsigned lane, std::size_t, std::size_t) {
            started.arrive_and_wait();
            if (lane == 1) {
                helper_completed = true;
                throw std::runtime_error("claimed helper");
            }
        });
        require(false, "A claimed helper's exception must reach the owner");
    } catch (const std::runtime_error& error) {
        require(helper_completed && std::string(error.what()) == "claimed helper", "The owner must join claimed helpers");
    }
    pool.run(1, [](unsigned lane, std::size_t begin, std::size_t end) {
        require(lane == 0 && begin == 0 && end == 1, "A failed shared queue must allow the next fixed dispatch");
    });
}

void parallel_world() {
    World initial;
    initial.map.width = 28;
    initial.map.height = 20;
    initial.map.tiles.resize(560);
    initial.map.starts = {Cell{3, 4}, Cell{24, 14}};
    std::string error;
    const auto rules = load_rules("tests/fixtures/coastal_rules.tsv", error);
    require(rules.has_value(), "Could not load original RTS fixture rules");
    initial.rules = *rules;
    initial.rng_state = 0x8cb97;
    const auto first = initial.spawn(UnitKind::Pioneer, Side::Blue, {4, 5}).value();
    const auto second = initial.spawn(UnitKind::Pioneer, Side::Blue, {6, 5}).value();
    const Command collision{.kind = CommandKind::Move, .side = Side::Blue, .target = {5, 5},
                            .units = {first, second}};
    require(initial.apply(collision), "Two units must be able to share a destination");
    for (int y = 6; y < 14; ++y) {
        const auto fighter = initial.spawn(UnitKind::Skirmisher, Side::Blue, {10, y}).value();
        initial.spawn(UnitKind::Skirmisher, Side::Copper, {14, y}).value();
        const Command advance{.kind = CommandKind::AttackMove, .side = Side::Blue, .target = {19, y},
                              .units = {fighter}};
        require(initial.apply(advance), "A combat unit must accept the attack order");
    }

    auto single = initial;
    auto parallel = initial;
    Navigator single_routes;
    Navigator parallel_routes(8);
    for (int step = 0; step < 84; ++step) {
        single.tick(single_routes);
        parallel.tick(parallel_routes);
        require(encode_snapshot(single) == encode_snapshot(parallel),
                "The complete simulation state must match at every step");
        for (const auto side : {Side::Blue, Side::Copper}) {
            require(std::ranges::equal(single_routes.visibility(side), parallel_routes.visibility(side)),
                    "Faction visibility must match across lane counts");
        }
        const auto& work = parallel_routes.last_work();
        require(work.active == work.moved + work.engaged && work.fired <= work.engaged,
                "Work metrics count unique active units and firing among engaged units");
        if (!step) {
            require((parallel.find(first)->position == Cell{5, 5}) &&
                        (parallel.find(second)->position == Cell{6, 5}),
                    "The lower entity ID must win a contested destination");
            const auto copper = parallel_routes.visibility(Side::Copper);
            require(copper[initial.map.index({15, 9})] && !copper[initial.map.index({1, 1})],
                    "The opposing side cannot see outside its authored unit ranges");
        }
    }
    require(single.rng_state != initial.rng_state, "The comparison must exercise actual firing");
}

}  // namespace

int main() {
    dispatch_and_reuse();
    join_on_error();
    alternating_participants_and_errors();
    shared_queue_dispatch();
    parallel_world();
    std::cout << "RTS worker tests passed (persistent lanes, exception joins, visibility and ordered combat)\n";
}
