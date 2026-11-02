#include "navigation/flow_cache.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <queue>
#include <random>
#include <stdexcept>
#include <thread>
#include <utility>

using coastal::navigation::FlowCache;
namespace {
struct Check {std::string label; bool passed;};
std::vector<Check> checks;
std::map<std::string,std::uint64_t> evidence;
void check(const std::string& label,bool passed) {
    checks.push_back({label,passed});
    if (!passed) std::cerr<<"FAIL "<<label<<'\n';
}
void require(bool passed,const std::string& error) {
    if (!passed) throw std::runtime_error(error);
}
bool cross(const std::vector<unsigned char>& grid,int a,int b,int width,int height) {
    if(a<0||b<0||a>=width*height||b>=width*height||grid[a]||grid[b])return false;
    const int x=a%width,z=a/width,nx=b%width,nz=b/width;
    if(std::abs(nx-x)>1||std::abs(nz-z)>1)return false;
    return nx==x||nz==z||(!grid[z*width+nx]&&!grid[nz*width+x]);
}
// Frozen pre-cache simulation algorithm, used as a compatibility oracle. The
// cache itself cannot provide the expected values or reference motion trace.
std::vector<std::int32_t> legacy(const std::vector<unsigned char>& grid,int goal,int width,int height) {
    std::vector<std::int32_t> distance(width*height,-1);
    if(grid[goal])return distance;
    using Item=std::pair<int,int>;
    std::priority_queue<Item,std::vector<Item>,std::greater<Item>> queue;
    queue.push({0,goal});distance[goal]=0;
    while(!queue.empty()){
        const auto [cost,cell]=queue.top();queue.pop();
        if(cost!=distance[cell])continue;
        const int x=cell%width,z=cell/width;
        for(int dz=-1;dz<=1;++dz)for(int dx=-1;dx<=1;++dx){
            if((!dx&&!dz)||x+dx<0||x+dx>=width||z+dz<0||z+dz>=height)continue;
            const int next=(z+dz)*width+x+dx;
            if(!cross(grid,cell,next,width,height))continue;
            const int candidate=cost+((dx&&dz)?1414:1000);
            if(distance[next]<0||candidate<distance[next]){
                distance[next]=candidate;queue.push({candidate,next});
            }
        }
    }
    return distance;
}
int next_cell(const std::vector<unsigned char>& grid,const std::vector<std::int32_t>& field,
              int cell,int goal,int width,int height) {
    if(field[cell]<=0)return cell;
    int result=cell,cost=std::numeric_limits<int>::max(),alignment=cost;
    const int x=cell%width,z=cell/width;
    for(int dz=-1;dz<=1;++dz)for(int dx=-1;dx<=1;++dx){
        if((!dx&&!dz)||x+dx<0||x+dx>=width||z+dz<0||z+dz>=height)continue;
        const int neighbour=(z+dz)*width+x+dx;
        if(!cross(grid,cell,neighbour,width,height)||field[neighbour]<0||field[neighbour]>=field[cell])continue;
        const int candidate=field[neighbour]+((dx&&dz)?1414:1000);
        const int vx=neighbour%width-goal%width,vz=neighbour/width-goal/width;
        const int aim=vx*vx+vz*vz;
        if(candidate<cost||(candidate==cost&&aim<alignment)){
            result=neighbour;cost=candidate;alignment=aim;
        }
    }
    return result;
}
std::vector<unsigned char> map_grid(int width,int height) {
    std::vector<unsigned char> grid(width*height,0);
    for(int z=0;z<height;++z)if(z!=height/4&&z!=3*height/4)grid[z*width+width/2]=1;
    for(int x=0;x<width;++x)if(x!=width/4&&x!=3*width/4)grid[(height/2)*width+x]=1;
    return grid;
}
std::string quote(const std::string& text) {
    std::string out="\"";
    for(char c:text){if(c=='\"'||c=='\\')out+='\\';if(c=='\n')out+="\\n";else out+=c;}
    return out+'\"';
}
void run() {
    std::string error;
    {
        FlowCache cache;
        auto grid=map_grid(17,19);
        require(cache.configure(17,19,grid,7,error),error);
        bool exact=true;
        std::mt19937 random(4081);
        for(int trial=0;trial<48;++trial){
            const int goal=int(random()%grid.size());
            std::vector<int> cells(grid.size());std::iota(cells.begin(),cells.end(),0);
            std::vector<std::int32_t> actual;
            exact=exact&&cache.sample(goal,cells,actual,error)&&actual==legacy(grid,goal,17,19);
        }
        check("Every cell matches the frozen original field on obstructed, corner and blocked-goal cases",exact);
        auto before=cache.export_active();
        auto invalid=grid;invalid[0]=2;
        check("Invalid grid and dimensions are rejected without resetting the live map",!cache.configure(7,19,grid,9,error)&&!cache.set_grid(invalid,9,error)&&cache.export_active().map_revision==before.map_revision);
        require(cache.set_active({1,1,2,-1},error),error);require(cache.prepare_active(error),error);
        const auto* retained=cache.find(1);
        check("Goal handles are stable identities and duplicate users share one pinned field",cache.handle_for_goal(1)==1&&cache.reference_count(1)==2&&cache.stats().active_goals==2);
        check("Bad references leave previous pins intact",!cache.set_active({-2},error)&&!cache.set_active({int(grid.size())},error)&&cache.reference_count(1)==2&&cache.find(1)==retained);
        const auto built=cache.stats().fields_built;
        require(cache.set_grid(grid,8,error),error);require(cache.prepare_active(error),error);
        check("A revision with byte-identical passability does not rebuild fields",cache.stats().fields_built==built&&cache.find(1)->map_revision==8);
        const auto resident=cache.stats().resident_fields;
        grid[5*17+4]=1;
        require(cache.set_grid(grid,9,error),error);
        check("A small grid change repairs exact live fields while preserving active references",cache.find(1)!=nullptr&&cache.find(1)->map_revision==9&&cache.reference_count(1)==2&&cache.export_active().active_goals==std::vector<int>({1,2}));
        require(cache.prepare_active(error),error);
        check("Repaired active fields equal a fresh original field with no full rebuild",cache.stats().fields_built==built&&cache.stats().fields_repaired==resident&&cache.find(1)->distance==legacy(grid,1,17,19));
    }
    {
        const int width=64,height=64;
        auto grid=map_grid(width,height);
        const std::size_t field_bytes=grid.size()*sizeof(std::int32_t);
        FlowCache serial(field_bytes*2),parallel(field_bytes*2);
        require(serial.configure(width,height,grid,11,error),error);
        require(parallel.configure(width,height,grid,11,error),error);
        std::vector<int> references(1024);std::iota(references.begin(),references.end(),0);
        require(serial.set_active(references,error),error);require(parallel.set_active(references,error),error);
        require(serial.prepare_active(error,1),error);require(parallel.prepare_active(error,4),error);
        bool identical=true;
        for(int goal:references)identical=identical&&serial.find(goal)->distance==parallel.find(goal)->distance;
        check("1024 independent full fields are bit-identical with one and four preparation threads",identical&&parallel.stats().last_prepare_threads==4);
        check("Active distance memory is explicitly reported above the inactive soft budget",parallel.stats().pinned_fields==1024&&parallel.stats().pinned_distance_bytes==1024*field_bytes&&parallel.stats().inactive_distance_bytes==0);
        const auto built=parallel.stats().fields_built;
        const auto* first=parallel.find(references.front());
        bool hot=true;
        for(int tick=0;tick<80;++tick){
            require(parallel.set_active(references,error),error);require(parallel.prepare_active(error,4),error);
            hot=hot&&parallel.find(references.front())==first;
        }
        check("1024 active goals across 80 ticks cause zero rebuilds or pointer changes",hot&&parallel.stats().fields_built==built);
        std::atomic<bool> readers_exact{true};
        std::vector<std::thread> readers;
        for(int worker=0;worker<4;++worker)readers.emplace_back([&,worker]{
            const FlowCache& view=parallel;
            for(int i=worker;i<1024;i+=4)if(view.find(i)->distance!=serial.find(i)->distance)readers_exact=false;
        });
        for(auto& reader:readers)reader.join();
        check("Published active fields support four immutable reader ranges",readers_exact.load());
        auto metadata=parallel.export_active();
        evidence["metadata_goal_bytes_for_1024"]=metadata.active_goals.size()*sizeof(int);
        evidence["distance_bytes_for_1024_on_64_squared"]=parallel.stats().pinned_distance_bytes;
        check("Compact exported route metadata has only 1024 goal integers",metadata.active_goals==references&&metadata.active_goals.size()*sizeof(int)==4096);
        FlowCache restored(field_bytes*2);
        require(restored.restore_active(metadata,grid,references,error),error);
        check("Metadata restore does not eagerly allocate all derived distance arrays",restored.stats().resident_distance_bytes==0&&restored.stats().active_goals==1024);
        require(restored.prepare_active(error,4),error);
        bool restore_exact=true;
        for(int goal:references)restore_exact=restore_exact&&restored.find(goal)->distance==serial.find(goal)->distance;
        check("A restored compact state regenerates every complete field exactly",restore_exact);
        std::vector<int> uninterrupted(1024),resumed(1024);
        for(int i=0;i<1024;++i){
            int start=(i*37+1301)%(width*height);
            while(grid[start])start=(start+1)%(width*height);
            uninterrupted[i]=resumed[i]=start;
        }
        const auto original=uninterrupted;
        bool continuation=true;
        for(int tick=0;tick<40;++tick)for(int i=0;i<1024;++i){
            uninterrupted[i]=next_cell(grid,serial.find(i)->distance,uninterrupted[i],i,width,height);
            resumed[i]=next_cell(grid,restored.find(i)->distance,resumed[i],i,width,height);
            continuation=continuation&&uninterrupted[i]==resumed[i];
        }
        int moved=0;for(int i=0;i<1024;++i)moved+=original[i]!=uninterrupted[i];
        evidence["moving_routes_checked"]=std::uint64_t(moved);
        check("1024 saved routes continue 40 actual grid steps identically after regeneration",continuation&&moved>700);
        auto broken=metadata;broken.active_goals.pop_back();
        const auto retained_builds=restored.stats().fields_built;
        check("Mismatched serialized route references are rejected atomically",!restored.restore_active(broken,grid,references,error)&&restored.stats().fields_built==retained_builds&&restored.find(0)!=nullptr);
        broken=metadata;broken.active_goals[1]=broken.active_goals[0];
        check("Duplicate serialized goals are rejected",!restored.restore_active(broken,grid,references,error));
        broken=metadata;broken.version=2;
        check("Unknown metadata versions are rejected",!restored.restore_active(broken,grid,references,error));
        bool rotated=true;
        for(int round=1;round<=8;++round){
            for(int i=0;i<1024;++i)references[i]=(i+503*round)%(width*height);
            require(parallel.set_active(references,error),error);require(parallel.prepare_active(error,4),error);
            const auto stats=parallel.stats();
            rotated=rotated&&stats.pinned_fields==1024&&stats.inactive_fields<=2&&stats.inactive_distance_bytes<=field_bytes*2;
            for(int goal:references)rotated=rotated&&parallel.find(goal)!=nullptr&&parallel.find(goal)->goal==goal;
        }
        check("Eight rounds of 1024 changing destinations keep active identities and bound inactive history",rotated&&parallel.stats().evictions>1000);
        evidence["historical_evictions_during_rotation"]=parallel.stats().evictions;
        parallel.set_inactive_budget(0);
        const auto active_bytes=parallel.stats().pinned_distance_bytes;
        parallel.discard_inactive();
        check("Zero inactive budget never releases pinned routes",parallel.stats().resident_distance_bytes==active_bytes&&parallel.stats().pinned_fields==1024&&parallel.stats().inactive_fields==0);
        require(parallel.set_active({},error),error);
        check("Releasing all users frees every field at zero inactive budget",parallel.stats().resident_distance_bytes==0&&parallel.stats().resident_fields==0);
        std::vector<std::int32_t> sample;
        require(parallel.sample(17,{0,17,1000},sample,error),error);
        const auto expected=legacy(grid,17,width,height);
        check("Cold zero-budget host queries return exact values without retaining a large field",sample==std::vector<std::int32_t>({expected[0],expected[17],expected[1000]})&&parallel.stats().resident_fields==0);
        const auto previous_sample=sample;
        check("Bad sample coordinates leave caller output unchanged",!parallel.sample(17,{-1},sample,error)&&sample==previous_sample);
    }
    {
        auto grid=map_grid(48,48);
        FlowCache maximum(0);
        require(maximum.configure(48,48,grid,19,error),error);
        std::vector<int> refs(2200);std::iota(refs.begin(),refs.end(),0);
        require(maximum.set_active(refs,error),error);require(maximum.prepare_active(error,4),error);
        const auto count=maximum.stats().fields_built;
        for(int tick=0;tick<16;++tick)require(maximum.prepare_active(error,4),error);
        check("All 2200 live references remain pinned without rebuilding beyond a zero soft budget",maximum.stats().pinned_fields==2200&&maximum.stats().fields_built==count);
        refs.push_back(0);
        check("The live-reference hard bound rejects a 2201st user without dropping routes",!maximum.set_active(refs,error)&&maximum.stats().active_references==2200);
    }
    {
        auto grid=map_grid(256,256);
        FlowCache cache(256U*256U*sizeof(std::int32_t));
        require(cache.configure(256,256,grid,50,error),error);
        require(cache.set_active({0,1000,60000,65535},error),error);
        require(cache.prepare_active(error,4),error);
        bool exact=true;
        for(int goal:{0,1000,60000,65535})exact=exact&&cache.find(goal)->distance==legacy(grid,goal,256,256);
        check("Actual 256-square fields preserve the complete original integer distances",exact&&cache.stats().pinned_distance_bytes==4U*262144U);
        evidence["one_256_squared_field_bytes"]=262144;
        evidence["maximum_2200_active_distance_bytes_at_256_squared"]=2200ULL*262144ULL;
        evidence["default_inactive_budget_bytes"]=8U*1024U*1024U;
    }
}
}

int main() {
    const auto start=std::chrono::steady_clock::now();
    try {run();}
    catch(const std::exception& error){check(std::string("Unexpected failure: ")+error.what(),false);}
    const auto passed=std::count_if(checks.begin(),checks.end(),[](const Check& item){return item.passed;});
    std::cout<<"{\"all_passed\":"<<(passed==std::ptrdiff_t(checks.size())?"true":"false")
             <<",\"check_count\":"<<checks.size()<<",\"checks\":[";
    for(std::size_t i=0;i<checks.size();++i){if(i)std::cout<<',';
        std::cout<<"{\"label\":"<<quote(checks[i].label)<<",\"passed\":"<<(checks[i].passed?"true":"false")<<'}';}
    std::cout<<"],\"evidence\":{";
    bool comma=false;for(const auto& [key,value]:evidence){if(comma)std::cout<<',';comma=true;std::cout<<quote(key)<<':'<<value;}
    std::cout<<"},\"elapsed_seconds\":"<<std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()
             <<",\"scope\":\"Pure cache plus frozen legacy field/motion oracle; no Godot/native binding/GPU claim\"}\n";
    return passed==std::ptrdiff_t(checks.size())?0:1;
}
