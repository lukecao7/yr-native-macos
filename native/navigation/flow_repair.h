#pragma once
#include "packed_min_heap.h"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace coastal::navigation::detail {

struct RepairWork {
    std::uint64_t invalidated = 0;
    std::uint64_t relaxed = 0;
};

// An edge is undirected. Diagonals require both cardinal side cells, exactly
// as the original full-field Dijkstra; no corner cutting is introduced.
template<class Visit>
inline void for_flow_neighbour(int width, int height,
                               const std::vector<unsigned char>& grid,
                               int cell, Visit&& visit) {
    if (grid[cell]) return;
    const int x=cell%width,z=cell/width;
    for (int dz=-1;dz<=1;++dz) for (int dx=-1;dx<=1;++dx) {
        if ((!dx&&!dz)||x+dx<0||x+dx>=width||z+dz<0||z+dz>=height) continue;
        const int next=(z+dz)*width+x+dx;
        if (grid[next]||(dx&&dz&&(grid[z*width+x+dx]||grid[(z+dz)*width+x]))) continue;
        visit(next,std::int32_t((dx&&dz)?1414:1000));
    }
}

// Repair an already exact distance field after a small batch of topology
// changes. All affected edge endpoints lie within one cell of a changed cell.
//
// First remove distances whose old shortest-path DAG no longer reaches the
// goal. A finite distance is supported only by a strictly smaller distance,
// so disconnected cycles cannot keep each other alive or count to infinity.
// Alternate unchanged shortest paths remain valid and need no recomputation.
// Then run ordinary Dijkstra from valid boundary distances and newly opened
// edges. The result is the complete exact field, not a local approximation.
// The caller owns a private candidate; allocations cannot mutate live fields.
inline RepairWork repair_flow_field(int width,int height,
                                    const std::vector<unsigned char>& grid,
                                    const std::vector<int>& endpoints,
                                    int goal,std::vector<std::int32_t>& distance) {
    RepairWork work;
    if (grid[goal]) {
        for (auto& value:distance) if (value>=0) {value=-1;++work.invalidated;}
        return work;
    }
    std::vector<unsigned char> queued(grid.size(),0);
    std::vector<int> pending,invalidated;
    pending.reserve(endpoints.size());
    const auto enqueue=[&](int cell) {
        if (distance[cell]>=0 && cell!=goal && !queued[cell]) {
            pending.push_back(cell); queued[cell]=1;
        }
    };
    for (int cell:endpoints) enqueue(cell);
    while (!pending.empty()) {
        const int cell=pending.back();pending.pop_back();queued[cell]=0;
        if (distance[cell]<0) continue;
        bool supported=false;
        for_flow_neighbour(width,height,grid,cell,[&](int next,std::int32_t cost) {
            if (distance[next]>=0 && distance[next]+cost==distance[cell]) supported=true;
        });
        if (supported) continue;
        distance[cell]=-1;invalidated.push_back(cell);++work.invalidated;
        const int x=cell%width,z=cell/width;
        for (int dz=-1;dz<=1;++dz) for (int dx=-1;dx<=1;++dx) {
            if ((!dx&&!dz)||x+dx<0||x+dx>=width||z+dz<0||z+dz>=height) continue;
            enqueue((z+dz)*width+x+dx);
        }
    }

    PackedMinHeap4 frontier;
    if (distance[goal]!=0) {distance[goal]=0;frontier.emplace(0,goal);++work.relaxed;}
    const auto lower=[&](int cell) {
        if (grid[cell]||cell==goal) return;
        std::int32_t best=distance[cell];
        for_flow_neighbour(width,height,grid,cell,[&](int next,std::int32_t cost) {
            if (distance[next]>=0 && (best<0||distance[next]+cost<best)) best=distance[next]+cost;
        });
        if (best>=0 && (distance[cell]<0||best<distance[cell])) {
            distance[cell]=best;frontier.emplace(best,cell);++work.relaxed;
        }
    };
    for (int cell:invalidated) lower(cell);
    for (int cell:endpoints) lower(cell);
    while (!frontier.empty()) {
        const auto item=frontier.top();frontier.pop();
        const auto cost=item.first;const auto cell=item.second;
        if (cost!=distance[cell]) continue;
        for_flow_neighbour(width,height,grid,cell,[&](int next,std::int32_t edge_cost) {
            const std::int32_t candidate=cost+edge_cost;
            if (distance[next]<0||candidate<distance[next]) {
                distance[next]=candidate;frontier.emplace(candidate,next);++work.relaxed;
            }
        });
    }
    return work;
}
}
