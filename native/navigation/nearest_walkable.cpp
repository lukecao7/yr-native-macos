#include "nearest_walkable.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace coastal::navigation {
int nearest_walkable(const std::vector<unsigned char>& grid,int width,int height,
    float ox,float oz,float size,float x,float z,int start,std::size_t* visited){
    if(visited)*visited=0;
    if(width<=0||height<=0||grid.size()!=std::size_t(width)*height||start<0||start>=width*height
        ||!std::isfinite(x)||!std::isfinite(z)||!std::isfinite(ox)||!std::isfinite(oz)||!std::isfinite(size)||size<=0)return -1;
    if(!grid[start]){if(visited)*visited=1;return start;}
    const int cx=start%width,cz=start/width;int best=-1;float distance=std::numeric_limits<float>::infinity();
    const auto visit=[&](int vx,int vz){
        if(vx<0||vx>=width||vz<0||vz>=height)return;if(visited)++*visited;
        const int c=vz*width+vx;if(grid[c])return;
        // Keep float arithmetic identical to the simulation's historical scan.
        const float dx=ox+(vx+.5f)*size-x,dz=oz+(vz+.5f)*size-z,d=dx*dx+dz*dz;
        if(d<distance||(d==distance&&(best<0||c<best))){best=c;distance=d;}
    };
    const auto axis_bound=[&](float point,float origin,int low,int high){
        if(low>high)return std::numeric_limits<float>::infinity();
        const double ideal=(double(point)-origin)/size-.5;
        // Roundoff at large translated origins may alter a neighbouring centre.
        // Sampling both neighbours makes this bound conservative as well.
        const double bounded=std::clamp(std::floor(ideal),double(low)-2,double(high)+2);
        const int at=int(bounded);float bound=std::numeric_limits<float>::infinity();
        for(int delta=-1;delta<=2;++delta){const int c=std::clamp(at+delta,low,high);const float d=origin+(c+.5f)*size-point;bound=std::min(bound,d*d);}
        return bound;
    };
    for(int radius=1;radius<std::max(width,height);++radius){
        for(int vx=cx-radius;vx<=cx+radius;++vx){visit(vx,cz-radius);visit(vx,cz+radius);}
        for(int vz=cz-radius+1;vz<cz+radius;++vz){visit(cx-radius,vz);visit(cx+radius,vz);}
        if(best<0)continue;
        const int left=std::max(0,cx-radius),right=std::min(width-1,cx+radius),top=std::max(0,cz-radius),bottom=std::min(height-1,cz+radius);
        const float bound=std::min({axis_bound(x,ox,0,left-1),axis_bound(x,ox,right+1,width-1),axis_bound(z,oz,0,top-1),axis_bound(z,oz,bottom+1,height-1)});
        if(bound>distance)break;
    }
    return best;
}
}
