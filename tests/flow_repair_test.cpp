#include "navigation/flow_cache.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <numeric>
#include <queue>
#include <random>
#include <stdexcept>

using coastal::navigation::FlowCache;
namespace {
int checks=0,failures=0;
std::map<std::string,double> timings;
std::uint64_t compared=0;
void check(const char* label,bool result) {
    ++checks;if(!result){++failures;std::cerr<<"FAIL: "<<label<<'\n';}
}
void require(bool value,const std::string& error) {if(!value)throw std::runtime_error(error);}
// Independent frozen priority_queue Dijkstra, never calls repair helpers.
std::vector<std::int32_t> fresh(int w,int h,const std::vector<unsigned char>& grid,int goal) {
    std::vector<std::int32_t> result(w*h,-1);
    if(grid[goal])return result;
    using Item=std::pair<int,int>;
    std::priority_queue<Item,std::vector<Item>,std::greater<Item>> heap;
    result[goal]=0;heap.emplace(0,goal);
    while(!heap.empty()) {
        const auto [distance,cell]=heap.top();heap.pop();
        if(result[cell]!=distance)continue;
        const int x=cell%w,z=cell/w;
        for(int dz=-1;dz<=1;++dz)for(int dx=-1;dx<=1;++dx) {
            if((!dx&&!dz)||x+dx<0||x+dx>=w||z+dz<0||z+dz>=h)continue;
            const int next=(z+dz)*w+x+dx;
            if(grid[next]||(dx&&dz&&(grid[z*w+x+dx]||grid[(z+dz)*w+x])))continue;
            const int candidate=distance+((dx&&dz)?1414:1000);
            if(result[next]<0||candidate<result[next]) {result[next]=candidate;heap.emplace(candidate,next);}
        }
    }
    return result;
}
bool exact(FlowCache& cache,int w,int h,const std::vector<unsigned char>& grid,const std::vector<int>& goals) {
    for(int goal:goals) {
        const auto* field=cache.find(goal);
        const auto oracle=fresh(w,h,grid,goal);
        compared+=grid.size();
        if(!field||field->distance!=oracle) {
            if(field)for(std::size_t i=0;i<grid.size();++i)if(field->distance[i]!=oracle[i]) {
                std::cerr<<"goal "<<goal<<" cell "<<i<<" actual "<<field->distance[i]<<" expected "<<oracle[i]<<'\n';break;
            }
            return false;
        }
    }
    return true;
}
void run() {
    std::string error;
    {
        const int w=24,h=25;std::vector<unsigned char> grid(w*h,0);
        for(int z=0;z<h;++z)if(z!=12)grid[z*w+12]=1;
        const std::vector<int> goals={0,11,12,13,12*w+12,w*h-1};
        FlowCache cache;require(cache.configure(w,h,grid,1,error),error);
        require(cache.set_active(goals,error),error);require(cache.prepare_active(error),error);
        const auto builds=cache.stats().fields_built;
        grid[12*w+12]=1;require(cache.set_grid(grid,2,error),error);
        check("Closing the only bridge clears a disconnected shortest-path component exactly",exact(cache,w,h,grid,goals));
        grid[12*w+12]=0;require(cache.set_grid(grid,3,error),error);
        check("Reopening the only bridge recovers unreachable cells exactly",exact(cache,w,h,grid,goals));
        grid[0]=1;grid[w*h-1]=1;require(cache.set_grid(grid,4,error),error);
        check("Blocked goals produce all-unreachable fields without stale zero anchors",exact(cache,w,h,grid,goals));
        grid[0]=0;grid[w*h-1]=0;require(cache.set_grid(grid,5,error),error);
        check("Reopened goals propagate an exact field from a previously empty anchor",exact(cache,w,h,grid,goals));
        grid[w+1]=1;grid[w+2]=1;grid[2*w+1]=1;require(cache.set_grid(grid,6,error),error);
        check("New cardinal blockers remove diagonal corner edges and preserve exact costs",exact(cache,w,h,grid,goals));
        grid[w+1]=0;grid[3*w+3]=1;require(cache.set_grid(grid,7,error),error);
        check("One mixed opening and closing batch propagates both increases and decreases",exact(cache,w,h,grid,goals));
        check("Six small topology commits retain fields without full Dijkstra rebuilds",cache.stats().fields_built==builds&&cache.stats().fields_repaired==goals.size()*6);
        const auto* retained=cache.find(0);const auto metadata=cache.export_active();const auto repairs=cache.stats().fields_repaired;
        require(cache.set_grid(grid,8,error),error);
        check("Revision-only changes retain field addresses and perform zero repairs",cache.find(0)==retained&&cache.stats().fields_repaired==repairs&&cache.find(0)->map_revision==8);
        auto invalid=grid;invalid[3]=2;
        check("Invalid topology is rejected before mutating fields, refs or metadata",!cache.set_grid(invalid,9,error)&&cache.find(0)==retained&&cache.export_active().active_goals==metadata.active_goals&&cache.export_active().map_revision==8&&exact(cache,w,h,grid,goals));
    }
    {
        std::mt19937 random(902104);
        bool all=true;
        for(int scenario=0;scenario<24&&all;++scenario) {
            const int w=8+int(random()%30),h=8+int(random()%28);
            std::vector<unsigned char> grid(w*h,0);
            for(auto& cell:grid)cell=(random()%100<static_cast<unsigned>(scenario%4)*18);
            std::vector<int> goals;
            for(int i=0;i<9;++i)goals.push_back(int(random()%grid.size()));
            FlowCache cache;require(cache.configure(w,h,grid,1,error),error);
            require(cache.set_active(goals,error),error);require(cache.prepare_active(error),error);
            for(int round=0;round<120&&all;++round) {
                const int changes=1+int(random()%12);
                for(int i=0;i<changes;++i)grid[random()%grid.size()]^=1;
                require(cache.set_grid(grid,round+2,error),error);
                require(cache.prepare_active(error),error);
                all=exact(cache,w,h,grid,goals);
                if(!all)std::cerr<<"random scenario "<<scenario<<" round "<<round<<'\n';
            }
        }
        check("2880 deterministic mixed random topology batches match fresh Dijkstra at every cell",all);
    }
    {
        const int w=32,h=32;std::vector<unsigned char> grid(w*h,0);
        const std::size_t field_bytes=grid.size()*sizeof(std::int32_t);
        FlowCache cache(field_bytes*2);require(cache.configure(w,h,grid,1,error),error);
        require(cache.set_active({1,2},error),error);require(cache.prepare_active(error),error);
        std::vector<std::int32_t> sampled;require(cache.sample(10,{0},sampled,error),error);require(cache.sample(11,{0},sampled,error),error);
        grid[10*w+10]=1;require(cache.set_grid(grid,2,error),error);
        check("Both pinned and inactive cached fields repair exactly",exact(cache,w,h,grid,{1,2,10,11})&&cache.stats().inactive_fields==2&&cache.stats().inactive_distance_bytes==field_bytes*2);
        require(cache.sample(12,{0},sampled,error),error);
        check("Repair preserves inactive LRU eviction order and budget",cache.find(10)==nullptr&&cache.find(11)!=nullptr&&cache.find(12)!=nullptr&&cache.stats().inactive_fields==2);
        const auto repairs=cache.stats().fields_repaired;
        for(int i=100;i<180;++i)grid[i]=1;
        require(cache.set_grid(grid,3,error),error);
        check("Large terrain edits discard derived fields while retaining real reference authority",cache.stats().resident_fields==0&&cache.stats().fields_repaired==repairs&&cache.reference_count(1)==1&&cache.reference_count(2)==1);
        require(cache.prepare_active(error),error);
        check("Large-edit fallback regenerates the same exact complete fields",exact(cache,w,h,grid,{1,2}));
        auto metadata=cache.export_active();FlowCache restored;
        require(restored.restore_active(metadata,grid,{1,2},error),error);require(restored.prepare_active(error),error);
        check("Derived repairs leave compact save metadata and post-restore distances unchanged",restored.find(1)->distance==cache.find(1)->distance&&restored.find(2)->distance==cache.find(2)->distance&&restored.stats().fields_repaired==0);
    }
    {
        const int w=256,h=256;std::vector<unsigned char> grid(w*h,0);
        for(int x=24;x<232;x+=40)for(int z=30;z<226;z+=32)
            for(int dz=0;dz<4;++dz)for(int dx=0;dx<4;++dx)grid[(z+dz)*w+x+dx]=1;
        std::vector<int> goals;for(int i=0;i<320;++i)goals.push_back((i*199+1523)%(w*h));
        FlowCache repaired;require(repaired.configure(w,h,grid,1,error),error);
        require(repaired.set_active(goals,error),error);require(repaired.prepare_active(error),error);
        grid[127*w+127]=grid[127*w+128]=grid[128*w+127]=grid[128*w+128]=1;
        auto start=std::chrono::steady_clock::now();require(repaired.set_grid(grid,2,error),error);
        timings["repair_320_fields_ms"]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        FlowCache rebuilt;require(rebuilt.configure(w,h,grid,2,error),error);require(rebuilt.set_active(goals,error),error);
        start=std::chrono::steady_clock::now();require(rebuilt.prepare_active(error),error);
        timings["full_rebuild_320_fields_ms"]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        bool same=true;for(int goal:goals)same=same&&repaired.find(goal)->distance==rebuilt.find(goal)->distance;
        check("A real 256-square four-cell building commit repairs 320 whole fields bit-identically",same&&repaired.stats().fields_repaired==320&&repaired.stats().last_repair_threads==4);
        check("Repair retains all active distance bytes and has no extra serialized authority",repaired.stats().pinned_distance_bytes==320ULL*w*h*sizeof(std::int32_t)&&repaired.export_active().active_goals==rebuilt.export_active().active_goals);
    }
}
}
int main() {
    try{run();}catch(const std::exception& error){std::cerr<<error.what()<<'\n';++failures;}
    std::cout<<"{\"all_passed\":"<<(failures?"false":"true")<<",\"check_count\":"<<checks<<",\"failures\":"<<failures<<",\"compared_cells\":"<<compared<<",\"timings\":{";
    bool comma=false;for(const auto& [key,value]:timings){if(comma)std::cout<<',';comma=true;std::cout<<'\"'<<key<<"\":"<<value;}
    std::cout<<"},\"scope\":\"Pure exact-distance oracle; no gameplay FPS claim\"}\n";
    return failures?1:0;
}
