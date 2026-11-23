// SPDX-License-Identifier: GPL-3.0-only
// Self-authored grid, rules and actors exercise only the public native API.
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
void static_corners() {
    for(unsigned mode=0;mode<3;++mode) {
        auto w=board();const Cell from{4,4},goal{10,10},east{5,4},diagonal{5,5};
        const auto mover=add(w,from);move(w,mover,goal);Navigator nav;
        std::vector<std::uint8_t> mask(w.map.tiles.size());
        if(mode==0)w.map.at(east).terrain=Terrain::Rock;
        if(mode==1){mask[w.map.index(east)]=1;nav.set_obstacle_mask(mask);}
        if(mode==2)add(w,east,UnitKind::Hub);
        nav.prepare(w);const auto query=static_steps(nav,w,from,goal);
        require(!includes(query,diagonal),"static query cut terrain/overlay/building corner");
        const auto next=nav.next(w,from,goal,mover);require(!next||*next!=diagonal,"next cut static corner");
        advance_kinematics(w,nav);require(w.find(mover)->position!=diagonal,"Core cut static corner");
    }
    std::cout<<"SEGMENT_STATIC_CORNERS_PASS terrain=true overlay=true building=true\n";
}
void side_cells_keep_sidearound_semantics() {
    for(unsigned mode=0;mode<3;++mode) {
        auto w=board();const Cell from{4,4},goal{10,10},east{5,4},south{4,5},diagonal{5,5};
        const auto mover=add(w,from);EntityId owner=0;
        if(mode==0)owner=add(w,east);else owner=add(w,{12,1});
        move(w,mover,goal);Navigator nav;auto entries=bodies(w);
        // The producer independently labels an actual side body, an outgoing
        // source tail, and a reserved-next cell. A side cell alone is not an
        // intersecting segment and therefore must not become static topology.
        if(mode==1)entries.push_back({east,owner,true});
        if(mode==2)entries.push_back({south,owner,true});
        nav.set_transient_occupancy(entries);nav.prepare(w);const auto before=encode_snapshot(w);
        require(nav.next(w,from,goal,mover)==diagonal,"dynamic side cell unnecessarily blocked diagonal");
        const auto query=static_steps(nav,w,from,goal);require(includes(query,diagonal),"static ranking included a mobile claim");
        require(encode_snapshot(w)==before,"read-only query mutated authority");
        advance_kinematics(w,nav);require(w.find(mover)->position==diagonal,"Core lost dynamic side-around semantics");
    }
    std::cout<<"SEGMENT_DYNAMIC_CORNERS_PASS body=true sourceTail=true reservedNext=true staticOnlyRanking=true\n";
}
void active_segments_wait_and_release() {
    for(unsigned lanes:{1U,4U,8U})for(bool reverse:{false,true}) {
        auto w=board();EntityId mover=0,owner=0;
        if(reverse){owner=add(w,{4,5});mover=add(w,{4,4});}
        else {mover=add(w,{4,4});owner=add(w,{4,5});}
        const Cell from{4,4},goal{5,5};move(w,mover,goal);Navigator nav(lanes);
        const std::array segment{MobileSegment{{5,4},{4,5},owner}};
        for(unsigned tick=0;tick<7;++tick) {
            auto cells=bodies(w);cells.push_back({{5,4},owner,true});
            nav.set_transient_occupancy(cells);nav.set_transient_segments(segment);nav.prepare(w);
            require(nav.transient_crossed(from,goal,mover),"active opposite diagonal not published");
            require(!nav.transient_crossed(from,goal,owner),"owner could not ignore its own derived segment");
            require(!nav.next(w,from,goal,mover),"next crossed active source-to-next segment");
            require(includes(static_steps(nav,w,from,goal),goal),"active segment leaked into static ranking");
            advance_kinematics(w,nav);require(w.find(mover)->position==from&&w.find(mover)->order==OrderKind::Move,
                    "Core entered an active crossing or retired the live command");
        }
        nav.set_transient_occupancy(bodies(w));nav.set_transient_segments({});nav.prepare(w);
        require(!nav.transient_crossed(from,goal,mover)&&nav.next(w,from,goal,mover)==goal,
                "completed physical segment did not release the pending Move");
        advance_kinematics(w,nav);require(w.find(mover)->position==goal,"Core failed after real segment release");
    }
    std::cout<<"SEGMENT_ACTIVE_WAIT_PASS sourceToNext=true sevenIntervals=true inverseIds=true release=true lanes1v4v8=true\n";
}
void segment_ownership_and_validation() {
    auto w=board();const auto mover=add(w,{4,4}),owner=add(w,{4,5});Navigator nav;
    const auto stage=[&](std::span<const MobileSegment> rows){nav.set_transient_segments(rows);nav.prepare(w);};
    stage(std::array{MobileSegment{{5,4},{4,5},owner},MobileSegment{{4,5},{5,4},owner}});
    require(nav.transient_crossed({4,4},{5,5},mover)&&!nav.transient_crossed({4,4},{5,5},owner),"duplicate/reversed same-owner segment changed ownership");
    stage(std::array{MobileSegment{{5,4},{4,5},no_entity}});
    require(nav.transient_crossed({4,4},{5,5},mover)&&nav.transient_crossed({4,4},{5,5},owner),"unknown segment owner did not conservatively block");
    const auto before=encode_snapshot(w);
    for(const auto bad:std::array{MobileSegment{{-1,4},{0,5},owner},MobileSegment{{5,4},{3,6},owner},MobileSegment{{5,4},{5,4},owner}}) {
        nav.set_transient_segments(std::array{bad});bool rejected=false;
        try{nav.prepare(w);}catch(const std::invalid_argument&){rejected=true;}
        require(rejected&&encode_snapshot(w)==before,"invalid segment was admitted or changed World");
    }
    stage({});require(!nav.transient_crossed({4,4},{5,5},mover),"cleared segment remained published");
    std::cout<<"SEGMENT_OWNERSHIP_PASS ownIgnore=true unknownConservative=true invalidRejected=true readOnly=true\n";
}
using History=std::vector<std::vector<std::uint8_t>>;
History traffic(unsigned lanes,bool orthogonal,bool reverse,bool head_on) {
    auto w=board();std::array<std::array<EntityId,2>,2> groups{};
    const std::array starts=head_on?
        std::array{Cell{6,7},Cell{3,10},Cell{7,7},Cell{12,10}}:
        std::array{Cell{2,6},Cell{2,8},orthogonal?Cell{6,2}:Cell{13,6},orthogonal?Cell{8,2}:Cell{13,8}};
    for(unsigned index=0;index<4;++index){const auto n=reverse?3-index:index;groups[n/2][n%2]=add(w,starts[n]);}
    const std::array goals=head_on?std::array{Cell{12,7},Cell{1,7}}:
        std::array{Cell{13,7},orthogonal?Cell{7,13}:Cell{2,7}};
    for(unsigned group=0;group<2;++group)move(w,groups[group],goals[group]);
    const auto ids=std::array{groups[0][0],groups[0][1],groups[1][0],groups[1][1]};
    const auto finished=[&]{for(unsigned g=0;g<2;++g)for(const auto id:groups[g]) {
        const auto* e=w.find(id);if(e->order!=OrderKind::Idle||std::max(std::abs(e->position.x-goals[g].x),std::abs(e->position.y-goals[g].y))>1)return false;
    }return true;};
    Navigator nav(lanes);History result;std::string error;unsigned finished_at=0;
    for(unsigned tick=0;tick<300;++tick) {
        if(tick==0||tick==7||tick==23) {
            const auto bytes=encode_snapshot(w);auto restored=decode_snapshot(bytes,error);
            require(bool(restored)&&encode_snapshot(*restored)==bytes,"traffic save/decode changed authority: "+error);w=std::move(*restored);
        }
        nav.set_transient_occupancy(bodies(w));nav.set_transient_segments({});
        const auto before=positions(w);advance_kinematics(w,nav);unique_and_separate(w,before);result.push_back(encode_snapshot(w));
        if(finished()&&!finished_at)finished_at=tick+1;
        if(finished_at){require(finished(),"settled group reactivated without command");require(before==positions(w)||tick+1==finished_at,"settled group moved again");}
    }
    require(finished_at>0&&finished_at<=120,"clear authored counterflow/crossing did not physically settle within bounded route");
    for(unsigned g=0;g<2;++g){unsigned at_goal=0;for(const auto id:groups[g])at_goal+=w.find(id)->position==goals[g];require(at_goal==1,"group lost exactly-one goal occupant");}
    for(const auto id:ids)require(w.find(id)->hp==100,"traffic damaged an authored actor");
    std::cout<<"SEGMENT_TRAFFIC_PASS lanes="<<lanes<<" orthogonal="<<orthogonal<<" reverse="<<reverse
             <<" adjacentHeadOn="<<head_on<<" settledAt="<<finished_at<<" stableAtLeast180=true\n";
    return result;
}
void full_traffic() {
    for(const bool orthogonal:{false,true})for(const bool reverse:{false,true})for(const bool head_on:{false,true}) {
        if(head_on&&orthogonal)continue;
        const auto one=traffic(1,orthogonal,reverse,head_on),four=traffic(4,orthogonal,reverse,head_on),eight=traffic(8,orthogonal,reverse,head_on);
        require(one==four&&one==eight,"traffic full snapshot bytes diverged between 1/4/8 lanes");
    }
}

void prepared_step_coherence() {
    for(unsigned lanes:{1U,4U,8U})for(bool reverse:{false,true}) {
        auto w=board();EntityId a=0,b=0;
        if(reverse){b=add(w,{7,7});a=add(w,{6,7});}
        else {a=add(w,{6,7});b=add(w,{7,7});}
        move(w,a,{12,7});move(w,b,{1,7});Navigator nav(lanes);
        nav.set_transient_occupancy(bodies(w));nav.prepare(w);
        const auto a_next=nav.next(w,w.find(a)->position,w.find(a)->destination,a);
        const auto b_next=nav.next(w,w.find(b)->position,w.find(b)->destination,b);
        require(a_next&&b_next,"head-on first-step fixture has no legal candidates");
        const auto before=positions(w);const auto bytes=encode_snapshot(w);
        nav.set_prepared_move_steps(std::array{PreparedMoveStep{a,a_next},PreparedMoveStep{b,b_next}});nav.prepare(w);
        require(encode_snapshot(w)==bytes,"prepared-step staging changed World");
        advance_kinematics(w,nav);unique_and_separate(w,before);
        for(const auto& [id,next]:std::array{std::pair{a,*a_next},std::pair{b,*b_next}}) {
            const auto from=before[id-1];
            require(w.find(id)->position==from||w.find(id)->position==next,
                    "Core committed an alternative that its external body controller had not prepared");
        }
        require((w.find(a)->position==before[a-1])!=(w.find(b)->position==before[b-1]),
                "stable owner arbitration should admit one prepared diagonal and hold the crossing counterpart");
        // A fresh preparation may choose a different step. Old choice is
        // derived scratch, and clearing it must expose normal alternatives.
        nav.set_prepared_move_steps({});nav.prepare(w);
        require(nav.prepared_step_allowed(a,{2,2})&&nav.prepared_step_allowed(b,{3,3}),
                "previous preparation leaked a saved or stale step constraint");
        nav.set_prepared_move_steps(std::array{PreparedMoveStep{a,std::nullopt}});nav.prepare(w);
        const auto from=w.find(a)->position;require(!nav.next(w,from,w.find(a)->destination,a),"empty prepared next admitted a path");
        advance_kinematics(w,nav);require(w.find(a)->position==from,"empty prepared next allowed Core motion");
        std::cout<<"PREPARED_STEP_PASS lanes="<<lanes<<" reverse="<<reverse
                 <<" noUnturnedAlternative=true crossingLoserWaits=true clearFreshPreparation=true emptyMeansWait=true\n";
    }
}

}  // namespace

int main() {
    try {
        static_corners();
        side_cells_keep_sidearound_semantics();
        active_segments_wait_and_release();
        segment_ownership_and_validation();
        full_traffic();
        prepared_step_coherence();
        std::cout << "MOBILE_SEGMENTS_SUITE_PASS selfAuthoredOnly=true "
                     "endpointAndInteriorSafe=true preparedStepCoherent=true "
                     "saveReplay1v4v8EveryTick=true\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "MOBILE_SEGMENTS_SUITE_FAIL " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
