#pragma once
#include <cstddef>
#include <vector>
namespace coastal::navigation {
// Exact Euclidean nearest walkable cell, with the same lowest-index tie break
// as a row-major full scan. Expands locally and stops only after proving all
// unvisited columns/rows farther than the best candidate. No runtime cache.
int nearest_walkable(const std::vector<unsigned char>& blocked,int width,int height,
    float origin_x,float origin_z,float cell_size,float x,float z,int start,
    std::size_t* visited=nullptr);
}
