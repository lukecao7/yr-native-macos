// SPDX-License-Identifier: GPL-3.0-only
#include "rts_core/combat.h"
#include "rts_core/match.h"
#include "rts_core/snapshot.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
using namespace coastal::rts;
int bit(int dx,int dy){int raw=(dy+1)*3+dx+1;return raw<4?raw:raw-1;}
World fixture(){
 std::string error;auto source=open_match("build/rts_core/fixture","Basin.map",error);assert(source);
 World w=std::move(*source);w.map.width=w.map.height=16;w.map.tiles.assign(256,{});w.map.starts={Cell{1,1},Cell{14,14}};w.map.travel_edges.assign(256,255);
 w.entities.clear();w.next_id=1;w.projectiles.clear();w.step=0;w.result=MatchResult::Running;
 for(size_t n=0;n<2;++n){w.sides[n]=SideState{};w.sides[n].rally=w.map.starts[n];}
 w.rules.spec(UnitKind::Pioneer).move_period=1;
 for(int y=0;y<16;++y)for(int x=0;x<16;++x)for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){
  if(!dx&&!dy)continue;Cell a{x,y},b{x+dx,y+dy};if(!w.map.contains(b))continue;
  if((a.x<=7)!=(b.x<=7) && (a.y!=12 || b.y!=12))w.map.travel_edges[w.map.index(a)]&=~(1U<<bit(dx,dy));
 }
 auto blue=w.spawn(UnitKind::Pioneer,Side::Blue,{1,1});auto copper=w.spawn(UnitKind::Pioneer,Side::Copper,{14,14});assert(blue&&copper);
 Command c{};c.kind=CommandKind::Move;c.side=Side::Blue;c.target={14,1};c.units={*blue};assert(w.apply(c));return w;
}
std::vector<std::vector<std::uint8_t>> exercise(unsigned threads){
 auto w=fixture();Navigator nav(threads);std::vector<std::vector<std::uint8_t>> states;
 assert(!w.map.can_travel({7,1},{8,1}));assert(!w.map.can_travel({7,11},{8,12}));assert(w.map.can_travel({7,12},{8,12}));
 bool crossed=false;std::optional<World> loaded;Navigator replay(threads);
 for(int n=0;n<160;++n){Cell before=w.entities.front().position;w.tick(nav);Cell after=w.entities.front().position;
  if(before!=after){assert(w.map.can_travel(before,after));if((before.x<=7)!=(after.x<=7)){assert(before.y==12&&after.y==12);crossed=true;}}
  auto bytes=encode_snapshot(w);assert(bytes[4]==3&&bytes[5]==0);states.push_back(bytes);
  if(n==9){std::string error;loaded=decode_snapshot(bytes,error);assert(loaded&&loaded->map.travel_edges==w.map.travel_edges);}
  else if(n>9){loaded->tick(replay);assert(encode_snapshot(*loaded)==bytes);}
 }
 assert(crossed);assert((w.entities.front().position==Cell{14,1}));
 auto invalid=w;invalid.map.travel_edges.pop_back();bool rejected=false;try{nav.prepare(invalid);}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
 return states;
}

void check_single_endpoint_mask() {
 Map map;
 map.width=map.height=8;
 map.tiles.assign(64,{});
 map.travel_edges.assign(64,0);
 const Cell a{3,3},b{4,3};
 constexpr std::uint8_t east=1U<<4,west=1U<<3;
 map.travel_edges[map.index(a)]=east;
 map.travel_edges[map.index(b)]=west;
 assert(map.can_travel(a,b)&&map.can_travel(b,a));

 // Only A's east bit is removed. B still explicitly admits its west edge.
 map.travel_edges[map.index(a)]=0;
 assert(map.travel_edges[map.index(b)]==west);
 assert(!map.can_travel(a,b)&&!map.can_travel(b,a));
 coastal::navigation::FlowCache cache;
 std::string error;
 const std::vector<unsigned char> grid(64,0);
 assert(cache.configure(8,8,grid,1,error,map.travel_edges));
 std::vector<std::int32_t> distance;
 assert(cache.sample(map.index(b),{map.index(a)},distance,error));
 assert(distance==std::vector<std::int32_t>{-1});
 assert(cache.sample(map.index(a),{map.index(b)},distance,error));
 assert(distance==std::vector<std::int32_t>{-1});

 // Reopening the same endpoint invalidates both previously unreachable fields.
 map.travel_edges[map.index(a)]=east;
 assert(cache.set_grid(grid,2,error,map.travel_edges));
 assert(cache.sample(map.index(b),{map.index(a)},distance,error));
 assert(distance==std::vector<std::int32_t>{1000});
 assert(cache.sample(map.index(a),{map.index(b)},distance,error));
 assert(distance==std::vector<std::int32_t>{1000});
}

constexpr std::size_t parallel_movers=16;
constexpr int parallel_ticks=64;

World parallel_fixture() {
 auto w=fixture();
 w.map.width=32;
 w.map.height=64;
 w.map.tiles.assign(std::size_t(w.map.width)*w.map.height,{});
 w.map.starts={Cell{1,2},Cell{30,61}};
 w.map.travel_edges.assign(w.map.tiles.size(),0);
 // Each row is a separate corridor. This is a nonempty imported topology,
 // rather than an all-open mask that would leave the new path unexercised.
 for(int y=0;y<w.map.height;++y)for(int x=0;x<w.map.width;++x) {
  if(x)w.map.travel_edges[w.map.index({x,y})]|=1U<<3;
  if(x+1<w.map.width)w.map.travel_edges[w.map.index({x,y})]|=1U<<4;
 }
 w.entities.clear();
 w.next_id=1;
 w.projectiles.clear();
 w.step=0;
 w.result=MatchResult::Running;
 for(std::size_t i=0;i<w.sides.size();++i) {
  w.sides[i]=SideState{};
  w.sides[i].rally=w.map.starts[i];
 }
 for(std::size_t i=0;i<parallel_movers;++i) {
  const Cell start{1,2+3*static_cast<int>(i)};
  const auto id=w.spawn(UnitKind::Pioneer,Side::Blue,start);
  assert(id);
  Command command{};
  command.kind=CommandKind::Move;
  command.side=Side::Blue;
  command.target={27+static_cast<int>(i%3),start.y};
  command.units={*id};
  assert(w.apply(command));
 }
 assert(w.spawn(UnitKind::Pioneer,Side::Copper,w.map.starts[1]));
 return w;
}

std::vector<std::vector<std::uint8_t>> parallel_exercise(unsigned threads) {
 auto w=parallel_fixture();
 Navigator nav(threads,true),replay(threads);
 std::optional<World> loaded;
 std::vector<Cell> destinations;
 for(std::size_t i=0;i<parallel_movers;++i)destinations.push_back(w.entities[i].destination);
 std::vector<std::vector<std::uint8_t>> states;
 states.reserve(parallel_ticks);
 std::size_t peak_goals=0;
 const Cell gate{16,2};
 for(int n=0;n<parallel_ticks;++n) {
  // Change only one endpoint of one corridor while all 16 goals are active.
  // The saved replay begins while that gate is closed, then reopens it too.
  if(n==8||n==20) {
   const auto change=[&](World& world) {
    auto& edge=world.map.travel_edges[world.map.index(gate)];
    if(n==8)edge&=~(1U<<4);
    else edge|=1U<<4;
   };
   change(w);
   if(loaded)change(*loaded);
  }
  std::vector<Cell> before;
  for(const auto& entity:w.entities)before.push_back(entity.position);
  w.tick(nav);
  for(std::size_t i=0;i<w.entities.size();++i) {
   if(before[i]!=w.entities[i].position)assert(w.map.can_travel(before[i],w.entities[i].position));
  }
  peak_goals=std::max(peak_goals,nav.flow_stats().active_goals);
  if(n==0) {
   // 16 moving actors plus one idle actor give every one of the 8 owner/worker
   // lanes at least one moving actor in its nonoverlapping range.
   assert(nav.last_work().moved==parallel_movers);
   assert(nav.flow_stats().active_goals==parallel_movers);
  }
  auto bytes=encode_snapshot(w);
  states.push_back(bytes);
  if(n==15) {
   std::string error;
   loaded=decode_snapshot(bytes,error);
   assert(loaded&&error.empty()&&loaded->map.travel_edges==w.map.travel_edges);
  } else if(n>15) {
   loaded->tick(replay);
   assert(encode_snapshot(*loaded)==bytes);
  }
 }
 assert(peak_goals==parallel_movers);
 assert(nav.flow_stats().grid_invalidations>=2);
 for(std::size_t i=0;i<parallel_movers;++i)assert(w.entities[i].position==destinations[i]);
 const auto& usage=nav.workers().usage();
 assert(usage.size()==threads);
 std::vector<std::uint64_t> identities;
 for(const auto& worker:usage) {
  assert(worker.jobs>=parallel_ticks);
  if(worker.thread_id)identities.push_back(worker.thread_id);
 }
#if defined(__APPLE__) && defined(__MACH__)
 // macOS exposes native IDs; other platforms still assert executed lane jobs.
 assert(identities.size()==threads);
 std::sort(identities.begin(),identities.end());
 assert(std::adjacent_find(identities.begin(),identities.end())==identities.end());
#endif
 std::cout<<"topology short replay threads="<<threads<<" movers="<<parallel_movers
          <<" goals="<<peak_goals<<" participating_lanes="<<usage.size()
          <<" native_thread_ids="<<identities.size()<<" ticks="<<parallel_ticks<<'\n';
 return states;
}

int main(){
 check_single_endpoint_mask();
 auto one=exercise(1);assert(one==exercise(4));assert(one==exercise(8));
 auto parallel_one=parallel_exercise(1);
 assert(parallel_one==parallel_exercise(4));
 assert(parallel_one==parallel_exercise(8));
 coastal::navigation::FlowCache c;std::string error;std::vector<unsigned char> grid(256,0),edges(256,255);
 assert(c.configure(16,16,grid,1,error,edges));assert(c.set_active({0},error));assert(c.prepare_active(error));assert(c.find(0)->distance[255]>=0);
 auto metadata=c.export_active();coastal::navigation::FlowCache restored;assert(restored.restore_active(metadata,grid,{0},error,edges));
 auto bad=edges;bad.pop_back();assert(!c.set_grid(grid,2,error,bad));assert(c.find(0));
 std::fill(edges.begin(),edges.end(),0);assert(c.set_grid(grid,3,error,edges));assert(c.prepare_active(error));assert(c.find(0)->distance[255]==-1);
 std::fill(edges.begin(),edges.end(),255);assert(c.set_grid(grid,4,error,edges));assert(c.prepare_active(error));assert(c.find(0)->distance[255]>=0);
 std::cout<<"RTS topology passed: barrier detour, one-endpoint reciprocal edges, cache invalidation, V3 replay, 16-goal short 1/4/8 identity\n";
}
