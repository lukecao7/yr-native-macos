#include "navigation/nearest_walkable.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <vector>
using coastal::navigation::nearest_walkable;
int scan(const std::vector<unsigned char>& grid,int w,int h,float ox,float oz,float cell,float x,float z,int start){
    if(!grid[start])return start;int best=-1;float distance=std::numeric_limits<float>::infinity();
    for(int c=0;c<w*h;++c)if(!grid[c]){const float dx=ox+(c%w+.5f)*cell-x,dz=oz+(c/w+.5f)*cell-z,d=dx*dx+dz*dz;if(d<distance){best=c;distance=d;}}
    return best;
}
int main(){
    std::mt19937 random(0x4e41564du);bool exact=true;int failed=-1;
    for(int i=0;i<12000;++i){
        const int w=8+random()%89,h=8+random()%89;
        const float cell=(i%3==0?.25f:i%3==1?.7f:2.0f),ox=i%7==0?999900.0f:-127.25f,oz=i%11==0?-999900.0f:203.75f;
        const float x=ox+(int(random()%(w*400))-w*100)*cell/100,z=oz+(int(random()%(h*400))-h*100)*cell/100;
        const int cx=std::clamp(int(std::floor((x-ox)/cell)),0,w-1),cz=std::clamp(int(std::floor((z-oz)/cell)),0,h-1),start=cz*w+cx;
        std::vector<unsigned char> grid(w*h);for(auto& b:grid)b=random()%100<65;grid[start]=1;
        const int a=scan(grid,w,h,ox,oz,cell,x,z,start),b=nearest_walkable(grid,w,h,ox,oz,cell,x,z,start);
        if(a!=b){exact=false;failed=i;std::cerr<<"case "<<i<<" expected "<<a<<" got "<<b<<"\n";break;}
    }
    const int w=256,h=256,center=128*w+128;std::vector<unsigned char> grid(w*h,0);
    for(int z=125;z<=131;++z)for(int x=125;x<=131;++x)grid[z*w+x]=1;
    std::size_t visited=0;const int near=nearest_walkable(grid,w,h,-128,-128,1,.2f,.2f,center,&visited);
    const bool local=near==scan(grid,w,h,-128,-128,1,.2f,.2f,center)&&visited<200;
    std::fill(grid.begin(),grid.end(),1);const bool closed=nearest_walkable(grid,w,h,-128,-128,1,.2f,.2f,center)==-1;
    grid[0]=0;const bool distant=nearest_walkable(grid,w,h,-128,-128,1,.2f,.2f,center)==0;
    grid.assign(w*h,0);grid[center]=1;
    const bool tie=nearest_walkable(grid,w,h,-128,-128,1,.5f,.5f,center)==center-w;
    const bool good=exact&&local&&closed&&distant&&tie;
    std::cout<<"{\"all_passed\":"<<(good?"true":"false")<<",\"check_count\":5,\"randomized_scenarios\":12000,\"randomized_exact\":"<<(exact?"true":"false")
        <<",\"first_randomized_failure\":"<<failed<<",\"local_search_visits\":"<<visited<<",\"local_equals_scan\":"<<(local?"true":"false")
        <<",\"all_blocked_returns_none\":"<<(closed?"true":"false")<<",\"distant_only_cell_found\":"<<(distant?"true":"false")<<",\"lowest_id_tie_preserved\":"<<(tie?"true":"false")<<"}\n";
    return good?0:1;
}
