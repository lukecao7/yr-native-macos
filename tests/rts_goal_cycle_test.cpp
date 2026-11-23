// SPDX-License-Identifier: GPL-3.0-only
// Self-authored closed-goal-cycle and successful-prepare contract fixtures.
#include "rts_core/combat.h"
#include "rts_core/economy.h"
#include "rts_core/snapshot.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace coastal::rts;
void require(bool okay,const std::string& message) {if(!okay)throw std::runtime_error(message);}
World board() {
    World w;w.map.width=w.map.height=16;w.map.tiles.resize(256);w.map.starts={Cell{0,0},Cell{15,15}};
    w.rules.initial_funds=1000;w.rules.cargo_capacity=8;w.rules.harvest_ticks=2;
    for(unsigned n=0;n<static_cast<unsigned>(UnitKind::Count);++n) {
        const auto kind=static_cast<UnitKind>(n);auto& s=w.rules.spec(kind);
        s.hp=100;s.cost=kind==UnitKind::Pioneer||kind==UnitKind::Hub?0:10;
        s.train_ticks=is_producible(kind)?2:0;s.move_period=is_building(kind)?0:1;s.vision=3;
    }
    w.rules.spec(UnitKind::Hub).power=2;w.rules.spec(UnitKind::Generator).power=10;
    w.rules.spec(UnitKind::Depot).power=-1;w.rules.spec(UnitKind::Workshop).power=-1;
    for(unsigned n=0;n<2;++n){w.sides[n].funds=1000;w.sides[n].rally=w.map.starts[n];}
    require(valid_rules(w.rules),"invalid self-authored rules");return w;
}
EntityId add(World& w,Cell cell,UnitKind kind=UnitKind::Sentinel) {
    const auto id=w.spawn(kind,Side::Blue,cell);require(bool(id),"self-authored spawn failed");return *id;
}
void move(World& w,std::span<const EntityId> ids,Cell goal) {
    Command select{};select.kind=CommandKind::Select;select.side=Side::Blue;select.units.assign(ids.begin(),ids.end());
    require(w.apply(select),"Select rejected");Command command{};command.kind=CommandKind::Move;
    command.side=Side::Blue;command.target=goal;require(w.apply(command),"selected Move rejected");
}
void move(World& w,EntityId id,Cell goal) {const std::array ids{id};move(w,ids,goal);}
// Exercise the public combat/navigation phase also called by World::tick.
// These one-side mechanism fixtures omit economy and match-victory handling,
// then advance the public step once for the next snapshot interval.
void advance_kinematics(World& w,Navigator& nav) {
    require(w.result==MatchResult::Running,"authored mechanism fixture is no longer running");
    tick_combat(w,nav);
    ++w.step;
}
std::vector<MobileOccupant> bodies(const World& w) {
    std::vector<MobileOccupant> out;
    for(const auto& e:w.entities)out.push_back({e.position,e.id,
        (e.order==OrderKind::Move||e.order==OrderKind::AttackMove)&&e.position!=e.destination});
    return out;
}
bool intersects(Cell a0,Cell a1,Cell b0,Cell b1) {
    const double x=a0.x-b0.x,y=a0.y-b0.y;
    const double dx=(a1.x-a0.x)-(b1.x-b0.x),dy=(a1.y-a0.y)-(b1.y-b0.y);
    const double norm=dx*dx+dy*dy,t=norm?std::clamp(-(x*dx+y*dy)/norm,0.0,1.0):0.0;
    return std::hypot(x+t*dx,y+t*dy)<1e-9;
}
std::vector<Cell> positions(const World& w) {
    std::vector<Cell> result;for(const auto& e:w.entities)result.push_back(e.position);return result;
}
void unique_and_separate(const World& w,const std::vector<Cell>& before) {
    for(std::size_t a=0;a<w.entities.size();++a)for(std::size_t b=a+1;b<w.entities.size();++b) {
        require(w.entities[a].position!=w.entities[b].position,"native endpoint overlap");
        require(!intersects(before[a],w.entities[a].position,before[b],w.entities[b].position),
                "same-interval native centre segments cross");
    }
}
std::vector<Cell> static_steps(Navigator& nav,const World& w,Cell from,Cell goal) {
    std::array<Cell,8> out;const auto n=nav.ranked_static_steps(w,from,goal,out);return {out.begin(),out.begin()+n};
}
bool includes(const std::vector<Cell>& cells,Cell at) {return std::find(cells.begin(),cells.end(),at)!=cells.end();}
using History=std::vector<std::vector<std::uint8_t>>;

void stage_bodies(Navigator& nav,const World& w) {nav.set_transient_occupancy(bodies(w));nav.set_transient_segments({});nav.prepare(w);}
History closed_cycle(unsigned lanes,bool reverse,unsigned count) {
    auto w=board();const std::array starts{Cell{4,4},Cell{5,4},Cell{5,5},Cell{4,5}};
    std::array<EntityId,4> ids{};
    for(unsigned n=0;n<count;++n){const auto index=reverse?count-1-n:n;ids[index]=add(w,starts[index]);}
    for(unsigned n=0;n<count;++n)move(w,ids[n],starts[(n+1)%count]);
    Navigator nav(lanes);stage_bodies(nav,w);const auto bytes=encode_snapshot(w);
    for(unsigned n=0;n<count;++n) {
        const auto id=ids[n];const auto* e=w.find(id);const auto next=nav.next(w,e->position,e->destination,id);
        if(id==1) {
            require(bool(next),"minimum-ID member has no safe equal-field escape");
            require(!includes(static_steps(nav,w,e->position,e->destination),*next),"equal-field escape leaked into strict static ranking");
        } else require(!next,"a second cycle member independently initiated an escape");
    }
    require(encode_snapshot(w)==bytes,"cycle owner query changed canonical state");History history;unsigned finished_at=0;
    for(unsigned tick=0;tick<300;++tick) {
        if(tick==9||tick==31) {
            std::string error;const auto checkpoint=encode_snapshot(w);auto restored=decode_snapshot(checkpoint,error);
            require(restored&&encode_snapshot(*restored)==checkpoint,"cycle save/load changed authority: "+error);w=std::move(*restored);
        }
        stage_bodies(nav,w);const auto before=positions(w);advance_kinematics(w,nav);unique_and_separate(w,before);
        bool finished=true;
        for(unsigned n=0;n<count;++n) {
            const auto* e=w.find(ids[n]);const auto goal=starts[(n+1)%count];
            if(e->order==OrderKind::Move){finished=false;require(e->destination==goal,"cycle detour changed original command destination");}
            else require(e->order==OrderKind::Idle&&e->position==goal,"cycle completed beside another actor instead of reaching its unique original goal");
        }
        if(finished&&!finished_at)finished_at=tick+1;
        if(finished_at){require(finished,"completed cycle reactivated");require(before==positions(w)||tick+1==finished_at,"completed cycle scattered again");}
        history.push_back(encode_snapshot(w));
    }
    require(finished_at>0&&finished_at<=120,"resolvable goal cycle did not finish within bounded open arena");
    std::cout<<"CYCLE_CANONICAL_PASS nodes="<<count<<" lanes="<<lanes<<" reverse="<<reverse<<" arrivedExactly=true leaderOnly=true originalGoals=true settledAt="<<finished_at<<'\n';
    return history;
}
void settled_goal_is_not_a_cycle() {
    auto w=board();const auto a=add(w,{4,4}),b=add(w,{6,4}),winner=add(w,{5,4});const Cell goal{5,4};
    move(w,a,goal);move(w,b,goal);Navigator nav;
    const auto start=positions(w);
    for(unsigned tick=0;tick<9;++tick) {
        auto claims=bodies(w);for(auto& row:claims)if(row.owner==winner)row.blocks_completion=true;
        nav.set_transient_occupancy(claims);nav.prepare(w);
        require(!nav.next(w,w.find(a)->position,goal,a)&&!nav.next(w,w.find(b)->position,goal,b),"unfinished goal turn incorrectly triggered cycle escape");
        advance_kinematics(w,nav);require(positions(w)==start,"waiting at a shared goal moved the goal occupant or peers");
    }
    for(unsigned tick=0;tick<190;++tick){stage_bodies(nav,w);advance_kinematics(w,nav);require(positions(w)==start,"settled common goal was pushed or re-scattered");}
    require(w.find(a)->order==OrderKind::Idle&&w.find(b)->order==OrderKind::Idle&&w.find(winner)->order==OrderKind::Idle,"settled goal completion changed");
    std::cout<<"CYCLE_SHARED_GOAL_BOUNDARY_PASS actualTurnWait=true completionRelease=true winnerNeverMoved=true stable190=true\n";
}
void absent_closed_cycle_does_not_escape() {
    // Open chain ends in a settled Hold; it is not a dependency cycle.
    {
        auto w=board();const auto a=add(w,{4,4}),b=add(w,{5,4}),c=add(w,{6,4});
        move(w,a,{5,4});move(w,b,{6,4});w.find(c)->order=OrderKind::Hold;Navigator nav;stage_bodies(nav,w);
        require(!nav.next(w,{4,4},{5,4},a),"open goal chain incorrectly escaped as a cycle");
    }
    // A retained outgoing source is not an actual World body at the goal.
    {
        auto w=board();const auto a=add(w,{4,4}),b=add(w,{6,4});move(w,a,{5,4});move(w,b,{4,4});Navigator nav;
        auto claims=bodies(w);claims.push_back({{5,4},b,true});nav.set_transient_occupancy(claims);
        nav.set_transient_segments(std::array{MobileSegment{{5,4},{6,4},b}});nav.prepare(w);
        require(!nav.next(w,{4,4},{5,4},a),"source-only dependency was invented as an actual closed goal cycle");
    }
    // Opponent Move orders do not participate in friendly cycle arbitration.
    {
        auto w=board();const auto a=add(w,{4,4}),b=add(w,{5,4});move(w,a,{5,4});w.find(b)->side=Side::Copper;
        Command c{};c.kind=CommandKind::Move;c.side=Side::Copper;c.units={b};c.target={4,4};require(w.apply(c),"Copper fixture Move rejected");
        Navigator nav;stage_bodies(nav,w);require(!nav.next(w,{4,4},{5,4},a),"opponent was treated as a friendly closed-cycle peer");
    }
    std::cout<<"CYCLE_TRIGGER_BOUNDARIES_PASS openChain=false sourceTailOnly=false opposingSide=false\n";
}
void blocked_cycle_waits_safely() {
    auto w=board();for(auto& tile:w.map.tiles)tile.terrain=Terrain::Rock;
    w.map.at({4,4}).terrain=Terrain::Ground;w.map.at({5,4}).terrain=Terrain::Ground;
    const auto a=add(w,{4,4}),b=add(w,{5,4});move(w,a,{5,4});move(w,b,{4,4});Navigator nav;
    for(unsigned tick=0;tick<30;++tick){stage_bodies(nav,w);require(!nav.next(w,{4,4},{5,4},a),"closed cycle crossed a static wall to manufacture an exit");advance_kinematics(w,nav);}
    require(w.find(a)->position==Cell{4,4}&&w.find(b)->position==Cell{5,4}&&w.find(a)->order==OrderKind::Move&&w.find(b)->order==OrderKind::Move,
            "unresolvable enclosed cycle changed original bodies or commands");
    std::cout<<"CYCLE_NO_EXIT_BOUNDARY_PASS noFabricatedExit=true originalCommandsRetained=true\n";
}
void active_owner_waits_then_releases() {
    for(unsigned lanes:{1U,4U,8U})for(unsigned mode:{0U,1U,2U}) {
        auto w=board();const auto a=add(w,{4,4}),b=add(w,{5,4});move(w,a,{5,4});move(w,b,{4,4});Navigator nav(lanes);
        const auto owner=mode==2?a:b;
        const MobileSegment segment=mode==0?MobileSegment{{5,3},{5,4},b}:
            mode==1?MobileSegment{{4,3},{5,4},b}:MobileSegment{{3,4},{4,4},a};
        for(unsigned interval=0;interval<5;++interval) {
            auto claims=bodies(w);claims.push_back({segment.from,owner,true});nav.set_transient_occupancy(claims);
            nav.set_transient_segments(std::array{segment});nav.prepare(w);
            require(nav.transient_owner_moving(owner)&&!nav.transient_owner_moving(50000),"active segment owner index is incorrect");
            require(!nav.next(w,{4,4},{5,4},a),"cycle escaped while its own or another node still owned a real physical segment");
            advance_kinematics(w,nav);
            require(w.find(a)->position==Cell{4,4}&&w.find(b)->position==Cell{5,4},"busy-owner cycle moved before physical settlement");
            require(w.find(a)->destination==Cell{5,4}&&w.find(b)->destination==Cell{4,4},"busy-owner wait rewrote canonical destinations");
        }
        stage_bodies(nav,w);require(!nav.transient_owner_moving(owner)&&nav.next(w,{4,4},{5,4},a),"settled physical owner did not release the genuine closed cycle");
        std::cout<<"CYCLE_ACTIVE_OWNER_BOUNDARY_PASS lanes="<<lanes<<" mode="<<mode<<" cardinalAndDiagonal=true selfAndPeer=true deferredUntilSettlement=true\n";
    }
}
void canonical_move_wait_defers_cycle() {
    for(bool requester:{false,true}) {
        auto w=board();const auto a=add(w,{4,4}),b=add(w,{5,4});move(w,a,{5,4});move(w,b,{4,4});
        w.find(requester?a:b)->move_wait=3;Navigator nav;
        for(unsigned tick=0;tick<3;++tick) {
            stage_bodies(nav,w);require(!nav.next(w,{4,4},{5,4},a),"canonical movement/pre-turn wait did not defer goal-cycle escape");
            advance_kinematics(w,nav);require(w.find(a)->position==Cell{4,4}&&w.find(b)->position==Cell{5,4},"cycle moved while a canonical movement/pre-turn gate remained live");
        }
        stage_bodies(nav,w);require(bool(nav.next(w,{4,4},{5,4},a)),"expired canonical movement wait did not release the cycle");
    }
    std::cout<<"CYCLE_MOVE_WAIT_BOUNDARY_PASS ownAndPeer=true actualSavedTimer=true naturalDecrement=true release=true\n";
}
void indexed_cycle_membership_boundaries() {
    {
        auto w=board();const auto outsider=add(w,{3,4}),a=add(w,{4,4}),b=add(w,{5,4});
        move(w,outsider,{4,4});move(w,a,{5,4});move(w,b,{4,4});Navigator nav;stage_bodies(nav,w);
        require(!nav.next(w,{3,4},{4,4},outsider),"a path entering another actual cycle became its leader");
        require(bool(nav.next(w,{4,4},{5,4},a)),"an outside lower ID suppressed the actual cycle's own minimum-ID leader");
    }
    {
        auto w=board();const auto a=add(w,{4,4}),b=add(w,{5,4}),overlap=add(w,{12,12});
        move(w,a,{5,4});move(w,b,{4,4});w.find(overlap)->position={5,4};Navigator nav;stage_bodies(nav,w);
        require(!nav.next(w,{4,4},{5,4},a),"ambiguous actual goal bodies formed an invented unique dependency cycle");
    }
    {
        auto w=board();const auto a=add(w,{4,4}),b=add(w,{5,4});move(w,a,{5,4});move(w,b,{4,4});Navigator nav;
        nav.set_transient_occupancy(bodies(w));nav.set_transient_segments(std::array{MobileSegment{{12,12},{13,13},no_entity}});nav.prepare(w);
        require(!nav.transient_owner_moving(a)&&!nav.transient_owner_moving(b)&&nav.next(w,{4,4},{5,4},a),"an unknown distant segment was attributed to an unrelated cycle node");
    }
    std::cout<<"CYCLE_INDEX_BOUNDARIES_PASS enteringOtherCycle=false ambiguousActualBodies=false unknownDistantOwnerNotAttributed=true\n";
}


void active_owner_publication() {
    auto w=board();const auto a=add(w,{4,4}),b=add(w,{5,4});move(w,a,{5,4});move(w,b,{4,4});Navigator nav;
    nav.set_transient_occupancy(bodies(w));nav.prepare(w);const auto before=encode_snapshot(w);
    require(!nav.transient_owner_moving(b)&&nav.next(w,{4,4},{5,4},a),"initial prepared resting cycle is invalid");
    const MobileSegment active{{5,3},{5,4},b};
    nav.set_transient_segments(std::array{active});
    require(!nav.transient_owner_moving(b)&&nav.next(w,{4,4},{5,4},a),"staging segments changed the last successful owner snapshot early");
    nav.prepare(w);require(nav.transient_owner_moving(b)&&!nav.next(w,{4,4},{5,4},a),"successful prepare did not publish active-owner wait");
    nav.set_transient_segments({});
    require(nav.transient_owner_moving(b)&&!nav.next(w,{4,4},{5,4},a),"staging a clear released an active owner before prepare");
    nav.prepare(w);require(!nav.transient_owner_moving(b)&&nav.next(w,{4,4},{5,4},a),"successful clear did not release cycle escape");
    nav.set_transient_segments(std::array{active,MobileSegment{{3,4},{4,4},a},active});nav.prepare(w);
    require(nav.transient_owner_moving(a)&&nav.transient_owner_moving(b),"unordered duplicate owners were lost");
    nav.set_transient_segments(std::array{MobileSegment{{-1,4},{0,4},a}});bool rejected=false;
    try{nav.prepare(w);}catch(const std::invalid_argument&){rejected=true;}
    require(rejected&&nav.transient_owner_moving(a)&&nav.transient_owner_moving(b),"rejected prepare damaged last successful owner snapshot");
    nav.set_transient_segments({});nav.prepare(w);
    require(!nav.transient_owner_moving(a)&&!nav.transient_owner_moving(b)&&encode_snapshot(w)==before,"owner query/publication mutated authority or retained stale owner");
    std::cout<<"ACTIVE_OWNER_PUBLICATION_PASS stageWaitsForPrepare=true clearWaitsForPrepare=true rejectedPrepareRetainsSnapshot=true duplicateOwners=true worldUnchanged=true\n";
}

}  // namespace

int main() {
    try {
        for(unsigned nodes:{2U,4U})for(bool reverse:{false,true}) {
            const auto one=closed_cycle(1,reverse,nodes),four=closed_cycle(4,reverse,nodes),eight=closed_cycle(8,reverse,nodes);
            require(one==four&&one==eight,"canonical goal-cycle full snapshots differ at 1/4/8");
        }
        settled_goal_is_not_a_cycle();
        absent_closed_cycle_does_not_escape();
        blocked_cycle_waits_safely();
        active_owner_waits_then_releases();
        canonical_move_wait_defers_cycle();
        indexed_cycle_membership_boundaries();
        active_owner_publication();
        std::cout << "GOAL_CYCLE_SUITE_PASS resolvableCategoryOnly=true canonicalGoals=true "
                     "fullSnapshots1v4v8=true ownerPublication=true selfAuthoredOnly=true\n";
        return EXIT_SUCCESS;
    } catch(const std::exception& error) {
        std::cerr << "GOAL_CYCLE_SUITE_FAIL " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
