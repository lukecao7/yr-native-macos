// SPDX-License-Identifier: GPL-3.0-only
// Synthetic cold-field benchmark. Use the same fixture and this same source
// against each matching FlowCache source/header generation. Record compiler
// flags outside the program; --label is a receipt label, not flag detection.
#include "navigation/flow_cache.h"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using Clock=std::chrono::steady_clock;
using Field=std::vector<std::int32_t>;
constexpr int width=160,height=168,goal_count=32,starts_per_goal=8;
constexpr std::uint64_t hash_seed=14695981039346656037ULL;
struct Grid {std::vector<unsigned char> blocked,edges;};
void require(bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);}
unsigned number(std::string_view text) {
    unsigned value=0;const auto [end,ec]=std::from_chars(text.data(),text.data()+text.size(),value);
    require(ec==std::errc{}&&end==text.data()+text.size()&&value>0,"Expected a positive integer");return value;
}
std::uint64_t hash_word(std::uint64_t h,std::uint32_t word) {
    for(int shift=0;shift<32;shift+=8){h^=(word>>shift)&255;h*=1099511628211ULL;}return h;
}
std::string hexadecimal(std::uint64_t hash){std::ostringstream out;out<<std::hex<<std::setfill('0')<<std::setw(16)<<hash;return out.str();}

// Frozen direct topology/Dijkstra oracle, independent of production masks,
// heaps and neighbour helpers. Row-major dz/dx order and integer edge costs
// are retained so complete route ties are reproducible across implementations.
bool open(const Grid& grid,int a,int b) {
    const int x=a%width,y=a/width,nx=b%width,ny=b/width;
    if(grid.blocked[a]||grid.blocked[b])return false;
    if(x!=nx&&y!=ny&&(grid.blocked[y*width+nx]||grid.blocked[ny*width+x]))return false;
    const auto reciprocal=[&](int from,int to) {
        const int raw=(to/width-from/width+1)*3+to%width-from%width+1;
        const int bit=raw<4?raw:raw-1;
        return (grid.edges[from]&(1U<<bit))&&(grid.edges[to]&(1U<<(7-bit)));
    };
    if(!reciprocal(a,b))return false;
    return x==nx||y==ny||(reciprocal(a,y*width+nx)&&reciprocal(a,ny*width+x)&&reciprocal(b,y*width+nx)&&reciprocal(b,ny*width+x));
}
template<class Visit>void neighbours(const Grid& grid,int cell,Visit&& visit) {
    if(grid.blocked[cell])return;
    const int x=cell%width,y=cell/width;
    for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx) {
        if((!dx&&!dy)||x+dx<0||x+dx>=width||y+dy<0||y+dy>=height)continue;
        const int next=(y+dy)*width+x+dx;if(open(grid,cell,next))visit(next,(dx&&dy)?1414:1000);
    }
}
Field direct_field(const Grid& grid,int goal) {
    Field distance(width*height,-1);if(grid.blocked[goal])return distance;
    using Item=std::pair<std::int32_t,int>;
    std::priority_queue<Item,std::vector<Item>,std::greater<Item>> queue;
    distance[goal]=0;queue.emplace(0,goal);
    while(!queue.empty()) {
        const auto current=queue.top();const auto cost=current.first;const auto cell=current.second;
        queue.pop();if(distance[cell]!=cost)continue;
        neighbours(grid,cell,[&](int next,std::int32_t edge) {
            const auto candidate=cost+edge;
            if(distance[next]<0||candidate<distance[next]){distance[next]=candidate;queue.emplace(candidate,next);}
        });
    }return distance;
}
std::vector<int> route(const Grid& grid,const Field& field,int start,int goal) {
    std::vector<int> path{start};if(field[start]<0)return path;
    int current=start;
    for(int steps=0;current!=goal&&steps<width*height;++steps) {
        int best=current;std::int32_t cost=std::numeric_limits<std::int32_t>::max();
        neighbours(grid,current,[&](int next,std::int32_t edge) {
            if(field[next]>=0&&field[next]<field[current]&&field[next]+edge<cost){best=next;cost=field[next]+edge;}
        });
        require(best!=current,"Reachable frozen route stopped before its goal");
        path.push_back(best);current=best;
    }
    require(current==goal,"Frozen route exceeded its complete-field bound");return path;
}
std::uint64_t elapsed(Clock::time_point start){return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count());}
}
int main(int argc,char** argv) {
    try {
        unsigned threads=8,rounds=3;std::string label="unspecified-build";
        for(int arg=1;arg<argc;++arg) {
            const std::string_view value=argv[arg];
            if(value.starts_with("--threads="))threads=number(value.substr(10));
            else if(value.starts_with("--rounds="))rounds=number(value.substr(9));
            else if(value.starts_with("--label="))label=value.substr(8);
            else throw std::runtime_error("Use --threads=1|4|8 --rounds=N --label=build-label");
        }
        require(threads==1||threads==4||threads==8,"Use 1, 4 or 8 worker lanes");
        require(rounds<=20,"Keep a bounded, comparable cold-field diagnostic");
        require(label.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-")==std::string::npos,"Label must be a simple build identifier");
        std::mt19937 random(0x63A215U);
        Grid grid{std::vector<unsigned char>(width*height),std::vector<unsigned char>(width*height,255)};
        for(int cell=0;cell<width*height;++cell) {
            grid.blocked[cell]=random()%100<9;
            if(random()%23==0)grid.edges[cell]&=static_cast<unsigned char>(~(1U<<(random()%8)));
        }
        for(int x=23;x<width;x+=29)for(int y=0;y<height;++y)if(y%31<26)grid.blocked[y*width+x]=1;
        std::set<int> chosen;
        while(chosen.size()<goal_count){const int goal=int(random()%(width*height));if(!grid.blocked[goal])chosen.insert(goal);}
        const std::vector<int> goals(chosen.begin(),chosen.end());
        std::vector<Field> expected;expected.reserve(goals.size());
        std::vector<std::vector<int>> expected_paths;expected_paths.reserve(goals.size()*starts_per_goal);
        std::vector<std::vector<int>> starts(goals.size());
        std::uint64_t expected_hash=hash_seed,expected_route_hash=hash_seed,total_route_steps=0,reachable_routes=0;
        for(std::size_t i=0;i<goals.size();++i) {
            expected.push_back(direct_field(grid,goals[i]));expected_hash=hash_word(expected_hash,std::uint32_t(goals[i]));
            for(const auto value:expected.back())expected_hash=hash_word(expected_hash,std::uint32_t(value));
            for(int n=0;n<starts_per_goal;++n) {
                const int start=int(random()%(width*height));starts[i].push_back(start);
                expected_paths.push_back(route(grid,expected.back(),start,goals[i]));
                const auto& path=expected_paths.back();total_route_steps+=path.size()-1;reachable_routes+=path.back()==goals[i];
                expected_route_hash=hash_word(expected_route_hash,std::uint32_t(path.size()));
                for(const int cell:path)expected_route_hash=hash_word(expected_route_hash,std::uint32_t(cell));
            }
        }
        require(reachable_routes>=64&&total_route_steps>=1000,"Synthetic fixture did not exercise enough complete routes");
        std::vector<std::uint64_t> configure_ns,prepare_ns,total_ns;
        std::uint64_t final_hash=0,final_route_hash=0;
        for(unsigned round=0;round<rounds;++round) {
            coastal::navigation::FlowCache cache;std::string error;
            auto start=Clock::now();require(cache.configure(width,height,grid.blocked,1,error,grid.edges),error);configure_ns.push_back(elapsed(start));
            require(cache.set_active(goals,error),error);
            start=Clock::now();require(cache.prepare_active(error,threads),error);prepare_ns.push_back(elapsed(start));
            total_ns.push_back(configure_ns.back()+prepare_ns.back());
            require(cache.stats().fields_built==goal_count,"The diagnostic must build all 32 genuinely cold goals");
            std::uint64_t actual_hash=hash_seed,actual_route_hash=hash_seed;
            for(std::size_t i=0;i<goals.size();++i) {
                const auto* field=cache.find(goals[i]);require(field&&field->distance==expected[i],"Distance field differs from frozen direct Dijkstra");
                actual_hash=hash_word(actual_hash,std::uint32_t(goals[i]));
                for(const auto value:field->distance)actual_hash=hash_word(actual_hash,std::uint32_t(value));
                for(int n=0;n<starts_per_goal;++n) {
                    const auto actual=route(grid,field->distance,starts[i][n],goals[i]);
                    require(actual==expected_paths[i*starts_per_goal+std::size_t(n)],"Complete route differs from frozen direct topology");
                    actual_route_hash=hash_word(actual_route_hash,std::uint32_t(actual.size()));
                    for(const int cell:actual)actual_route_hash=hash_word(actual_route_hash,std::uint32_t(cell));
                }
            }
            require(actual_hash==expected_hash&&actual_route_hash==expected_route_hash,"Canonical distance/route hashes differ");
            final_hash=actual_hash;final_route_hash=actual_route_hash;
        }
        const auto array=[](const std::vector<std::uint64_t>& values){std::ostringstream out;out<<'[';for(std::size_t i=0;i<values.size();++i){if(i)out<<',';out<<values[i];}out<<']';return out.str();};
        std::cout<<"NAVIGATION_NEIGHBOUR_BENCH {\"label\":\""<<label<<"\",\"width\":"<<width<<",\"height\":"<<height
            <<",\"seed\":6529557,\"goals\":"<<goal_count<<",\"threads\":"<<threads<<",\"rounds\":"<<rounds
            <<",\"comparedCellsPerRound\":"<<width*height*goal_count<<",\"completeRoutesPerRound\":"<<expected_paths.size()
            <<",\"reachableRoutes\":"<<reachable_routes<<",\"routeStepsPerRound\":"<<total_route_steps
            <<",\"distanceHash\":\""<<hexadecimal(final_hash)<<"\",\"routeHash\":\""<<hexadecimal(final_route_hash)
            <<"\",\"distanceAndCompleteRoutesExact\":true,\"configureNs\":"<<array(configure_ns)<<",\"coldPrepareNs\":"<<array(prepare_ns)
            <<",\"totalConfigureAndPrepareNs\":"<<array(total_ns)<<",\"compilerFlagsExternallyRecorded\":true,\"fixture\":\"synthetic-only\"}\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<"NAVIGATION_NEIGHBOUR_BENCH_FAILED "<<error.what()<<'\n';return 1;}
}
