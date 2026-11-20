// SPDX-License-Identifier: GPL-3.0-only
#include "rts_core/combat.h"
#include "rts_core/economy.h"
#include "rts_core/snapshot.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace coastal::rts;

void require(bool okay,const std::string& message) {
    if (!okay) throw std::runtime_error(message);
}

// Entirely self-authored Coastal rules and geometry. No data files or private
// runtime entry points are needed, and the parallel case includes real combat.
Rules fixture_rules() {
    Rules rules;
    rules.initial_funds=1000;
    rules.cargo_capacity=8;
    rules.harvest_ticks=2;
    for (unsigned i=0;i<static_cast<unsigned>(UnitKind::Count);++i) {
        const auto kind=static_cast<UnitKind>(i);
        auto& spec=rules.spec(kind);
        spec.hp=1000;
        spec.cost=kind==UnitKind::Pioneer||kind==UnitKind::Hub?0:10;
        spec.train_ticks=is_producible(kind)?2:0;
        spec.move_period=is_building(kind)?0:1;
        spec.vision=3;
    }
    rules.spec(UnitKind::Hub).power=2;
    rules.spec(UnitKind::Generator).power=10;
    rules.spec(UnitKind::Depot).power=-1;
    rules.spec(UnitKind::Workshop).power=-1;
    auto& fighter=rules.spec(UnitKind::Skirmisher);
    fighter.range=2;
    fighter.damage=2;
    fighter.shot_period=7;
    require(valid_rules(rules),"Self-authored rules must satisfy the public rule contract");
    return rules;
}

World open_world(int width,int height) {
    World world;
    world.rules=fixture_rules();
    world.rng_state=0x91d74ac3;
    world.map.width=width;
    world.map.height=height;
    world.map.tiles.resize(std::size_t(width)*height);
    world.map.starts={Cell{0,0},Cell{width-1,height-1}};
    for (std::size_t side=0;side<world.sides.size();++side) {
        world.sides[side].rally=world.map.starts[side];
        world.sides[side].funds=world.rules.initial_funds;
    }
    return world;
}

EntityId spawn(World& world,UnitKind kind,Side side,Cell at) {
    const auto id=world.spawn(kind,side,at);
    require(id.has_value(),"Could not spawn a self-authored Coastal test unit");
    return *id;
}

void move(World& world,EntityId id,Cell goal) {
    Command command{};
    command.kind=CommandKind::Move;
    command.side=world.find(id)->side;
    command.target=goal;
    command.units={id};
    require(world.apply(command),"Public Move command was rejected");
}

bool masked(const World& world,const std::vector<std::uint8_t>& mask,Cell at) {
    return !mask.empty()&&mask[world.map.index(at)]!=0;
}

void check_step(const World& world,const std::vector<std::uint8_t>& mask,Cell from,Cell to) {
    require(world.map.contains(to)&&!masked(world,mask,to),"Unit entered an external obstacle");
    if (from==to) return;
    require(world.map.can_travel(from,to),"Unit took an invalid terrain edge");
    if (from.x!=to.x&&from.y!=to.y) {
        const Cell x{to.x,from.y},y{from.x,to.y};
        require(!masked(world,mask,x)&&!masked(world,mask,y),"Unit cut a masked diagonal corner");
    }
}

void live_overlay_and_cache() {
    auto world=open_world(16,16);
    world.map.at({0,0}).terrain=Terrain::Rock;
    world.map.at({0,1}).terrain=Terrain::Water;
    const auto unit=spawn(world,UnitKind::Pioneer,Side::Blue,{2,2});
    spawn(world,UnitKind::Hub,Side::Copper,{1,14});
    world.map.starts={Cell{2,2},Cell{1,14}};
    Navigator nav;
    const Cell goal{13,2};
    nav.prepare(world);
    require(nav.next(world,{2,2},goal)==Cell{3,2},"Unmasked straight route did not warm the cache");
    require(nav.flow_stats().resident_fields>0,"Test did not warm a flow field");

    std::vector<std::uint8_t> mask(world.map.tiles.size(),0);
    for (int y=0;y<15;++y) mask[world.map.index({8,y})]=1;
    nav.set_obstacle_mask(mask);
    nav.prepare(world);
    const auto revision=nav.flow_stats().grid_invalidations;
    require(revision>0,"Adding a wall did not invalidate the warm grid");
    nav.set_obstacle_mask(mask);
    nav.prepare(world);
    require(nav.flow_stats().grid_invalidations==revision,"An unchanged mask invalidated the grid");
    auto nonbinary=mask;
    for (auto& value:nonbinary) if (value) value=7;
    nav.set_obstacle_mask(nonbinary);
    nav.prepare(world);
    require(nav.flow_stats().grid_invalidations==revision,"Equivalent nonzero blockers invalidated the grid");
    move(world,unit,goal);
    bool crossed_gap=false;
    for (unsigned tick=0;tick<80;++tick) {
        const Cell before=world.find(unit)->position;
        world.tick(nav);
        const Cell after=world.find(unit)->position;
        check_step(world,mask,before,after);
        if (before.x<8&&after.x>=8) {
            require(before.y==15&&after.y==15,"Unit crossed the wall away from its open gap");
            crossed_gap=true;
        }
    }
    require(crossed_gap&&world.find(unit)->position==goal,"Unit did not detour through the open gap");

    // Reuse the warm goal, close the only passage, then reopen it. This must
    // repair/invalidate old distances rather than rely on a fresh Navigator.
    world.find(unit)->position={2,2};
    world.find(unit)->move_wait=0;
    mask[world.map.index({8,15})]=1;
    nav.set_obstacle_mask(mask);
    nav.prepare(world);
    require(!nav.next(world,{2,2},goal),"Closed wall reused an obsolete reachable field");
    move(world,unit,goal);
    for (unsigned tick=0;tick<24;++tick) {
        world.tick(nav);
        require(world.find(unit)->position.x<8,"Unit crossed the closed wall");
    }
    std::fill(mask.begin(),mask.end(),0);
    nav.set_obstacle_mask(mask);
    nav.prepare(world);
    require(nav.next(world,{2,2},goal)==Cell{3,2},"Zero mask did not reopen a cached unreachable route");

    for (int y=0;y<16;++y) mask[world.map.index({8,y})]=1;
    nav.set_obstacle_mask(mask);
    nav.prepare(world);
    require(!nav.next(world,{2,2},goal),"Second wall closure did not update the cache");
    nav.set_obstacle_mask({});
    nav.prepare(world);
    require(nav.next(world,{2,2},goal)==Cell{3,2},"Empty span did not clear the live overlay");
    const auto cleared_revision=nav.flow_stats().grid_invalidations;
    std::fill(mask.begin(),mask.end(),0);
    nav.set_obstacle_mask(mask);
    nav.prepare(world);
    require(nav.flow_stats().grid_invalidations==cleared_revision,"Empty and zero masks changed equivalent grids");
    require(!nav.next(world,{0,0},{1,0}),"Clearing the overlay removed base rock blocking");
    require(!nav.next(world,{0,1},{1,1}),"Clearing the overlay removed base water blocking");
    require(!nav.next(world,{1,14},{2,14}),"Clearing the overlay removed a live building anchor");
    for (unsigned tick=0;tick<24;++tick) world.tick(nav);
    require(world.find(unit)->position==goal,"Unit did not finish after clearing the overlay");
    std::cout<<"Obstacle masks: warm detour, close/reopen, empty clear and base blockers passed\n";
}

void padded_stride_copy_and_recovery() {
    auto world=open_world(3,5);
    Navigator nav;
    std::vector<std::uint8_t> expected(15,0);
    for (int y=0;y<4;++y) expected[world.map.index({1,y})]=1;
    {
        auto temporary=expected;
        nav.set_obstacle_mask(temporary);
        // The setter owns a copy even though publication is deferred.
        std::fill(temporary.begin(),temporary.end(),0);
    }
    nav.prepare(world);
    Cell cursor{0,0};
    const Cell goal{2,0};
    bool used_last_row=false;
    for (unsigned step=0;step<32&&cursor!=goal;++step) {
        const auto next=nav.next(world,cursor,goal);
        require(next.has_value(),"Copied 3x5 mask lost its valid bottom passage");
        check_step(world,expected,cursor,*next);
        cursor=*next;
        used_last_row|=cursor.y==4;
    }
    require(cursor==goal&&used_last_row,"Mask indexing used the padded flow stride instead of Map width");
    const auto before=encode_snapshot(world);
    for (const auto bad_size:{14U,40U}) {
        nav.set_obstacle_mask(std::vector<std::uint8_t>(bad_size,1));
        bool rejected=false;
        try {nav.prepare(world);} catch (const std::invalid_argument&) {rejected=true;}
        require(rejected,"Incorrect map/padded mask size was not rejected");
        require(encode_snapshot(world)==before,"Rejected prepare changed the World");
        nav.set_obstacle_mask({});
        nav.prepare(world);
        require(nav.next(world,{0,0},goal)==Cell{1,0},"Clear/prepare did not recover from a rejected mask");
    }
    std::cout<<"Obstacle masks: 3x5 stride, copied ownership and invalid-size recovery passed\n";
}

constexpr unsigned movers=16,parallel_ticks=96;

World parallel_world(bool explicit_edges) {
    auto world=open_world(32,64);
    for (auto& tile:world.map.tiles) tile.terrain=Terrain::Rock;
    for (unsigned mover=0;mover<movers;++mover) {
        const int row=2+3*static_cast<int>(mover);
        for (int x=0;x<32;++x) world.map.at({x,row}).terrain=Terrain::Ground;
        const auto id=spawn(world,UnitKind::Pioneer,Side::Blue,{1,row});
        move(world,id,{28+static_cast<int>(mover%3),row});
    }
    for (int x=0;x<32;++x) world.map.at({x,61}).terrain=Terrain::Ground;
    spawn(world,UnitKind::Pioneer,Side::Copper,{30,61});
    // A separate duel exercises ordered projectile commits and RNG as the
    // movement workers read the changing mask. Both survive the whole test.
    for (int x=4;x<=8;++x) world.map.at({x,55}).terrain=Terrain::Ground;
    spawn(world,UnitKind::Skirmisher,Side::Blue,{5,55});
    spawn(world,UnitKind::Skirmisher,Side::Copper,{7,55});
    world.map.starts={Cell{1,2},Cell{30,61}};
    world.sides[0].rally=world.map.starts[0];
    world.sides[1].rally=world.map.starts[1];
    if (explicit_edges) {
        // Cardinal-only rows are an actual nonempty imported topology.
        world.map.travel_edges.assign(world.map.tiles.size(),0);
        for (int y=0;y<64;++y) for (int x=0;x<32;++x) {
            if (x) world.map.travel_edges[world.map.index({x,y})]|=1U<<3;
            if (x+1<32) world.map.travel_edges[world.map.index({x,y})]|=1U<<4;
        }
    }
    return world;
}

std::vector<std::vector<std::uint8_t>> parallel_run(unsigned threads,bool explicit_edges) {
    auto world=parallel_world(explicit_edges);
    const auto initial_rng=world.rng_state;
    Navigator nav(threads,true);
    std::vector<std::uint8_t> mask(world.map.tiles.size(),0);
    nav.set_obstacle_mask(mask);
    std::vector<std::vector<std::uint8_t>> states;
    std::uint64_t repeated_revision=0;
    for (unsigned tick=0;tick<parallel_ticks;++tick) {
        if (tick==5||tick==32) {
            std::fill(mask.begin(),mask.end(),0);
            const int column=tick==5?12:22;
            for (unsigned mover=0;mover<movers;++mover)
                mask[world.map.index({column,2+3*static_cast<int>(mover)})]=1;
            nav.set_obstacle_mask(mask);
        } else if (tick==6) {
            repeated_revision=nav.flow_stats().grid_invalidations;
            nav.set_obstacle_mask(mask);
        } else if (tick==24) {
            std::fill(mask.begin(),mask.end(),0);
            nav.set_obstacle_mask(mask);
        } else if (tick==48) {
            std::fill(mask.begin(),mask.end(),0);
            nav.set_obstacle_mask({});
        }
        std::array<Cell,movers> before{};
        for (unsigned mover=0;mover<movers;++mover) before[mover]=world.find(mover+1)->position;
        world.tick(nav);
        require(world.result==MatchResult::Running,"Parallel fixture ended before exercising all updates");
        for (unsigned mover=0;mover<movers;++mover)
            check_step(world,mask,before[mover],world.find(mover+1)->position);
        if (tick==6) require(nav.flow_stats().grid_invalidations==repeated_revision,"Repeated parallel mask invalidated the cache");
        states.push_back(encode_snapshot(world));
    }
    for (unsigned mover=0;mover<movers;++mover) {
        const Cell goal{28+static_cast<int>(mover%3),2+3*static_cast<int>(mover)};
        require(world.find(mover+1)->position==goal,"Parallel mover did not resume and reach its own goal");
    }
    require(world.rng_state!=initial_rng,"Parallel comparison did not exercise actual combat RNG");
    require(nav.flow_stats().grid_invalidations>=4,"Parallel run did not publish every obstacle change");
    require(explicit_edges?nav.flow_stats().fields_repaired==0:nav.flow_stats().fields_repaired>0,
        "Empty/nonempty edge variants did not exercise their distinct cache update paths");
    for (const auto& lane:nav.workers().usage()) require(lane.jobs>0,"Requested worker lane never ran");
    return states;
}

void parallel_consistency() {
    for (const bool explicit_edges:{false,true}) {
        const auto single=parallel_run(1,explicit_edges);
        require(single==parallel_run(4,explicit_edges),"Complete snapshots diverged between one and four lanes");
        require(single==parallel_run(8,explicit_edges),"Complete snapshots diverged between one and eight lanes");
    }
    std::cout<<"Obstacle masks: 16 movers plus combat, 1/4/8 lanes, empty/nonempty edges matched every tick\n";
}

void masked_goal_waits() {
    auto world=open_world(16,16);
    const auto unit=spawn(world,UnitKind::Pioneer,Side::Blue,{2,2});
    spawn(world,UnitKind::Hub,Side::Copper,{1,14});
    Navigator nav;
    const Cell goal{13,2};
    nav.prepare(world);
    require(nav.next(world,{2,2},goal).has_value(),"Masked-goal test did not warm the original route");
    std::vector<std::uint8_t> mask(world.map.tiles.size(),0);
    mask[world.map.index(goal)]=1;
    nav.set_obstacle_mask(mask);
    move(world,unit,goal);
    for (unsigned tick=0;tick<48;++tick) world.tick(nav);
    const auto* actor=world.find(unit);
    const Cell waiting=actor->position;
    require(std::max(std::abs(waiting.x-goal.x),std::abs(waiting.y-goal.y))==1,
        "Move did not approach the masked goal from an accessible neighbor");
    require(actor->order==OrderKind::Move,"Mask-only target was mistaken for an entity-completed Move");
    for (unsigned tick=0;tick<8;++tick) {
        world.tick(nav);
        require(world.find(unit)->position==waiting,"Pending masked-goal Move wandered while waiting");
    }
    nav.set_obstacle_mask({});
    for (unsigned tick=0;tick<4;++tick) world.tick(nav);
    require(world.find(unit)->position==goal&&world.find(unit)->order==OrderKind::Idle,
        "Move did not complete after the mask-only goal was reopened");
    std::cout<<"Obstacle masks: masked-goal wait and completion after clearing passed\n";
}
} // namespace

int main() {
    try {
        live_overlay_and_cache();
        padded_stride_copy_and_recovery();
        parallel_consistency();
        masked_goal_waits();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr<<"RTS obstacle-mask test failed: "<<error.what()<<'\n';
        return EXIT_FAILURE;
    }
}
