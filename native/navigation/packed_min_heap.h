#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

namespace coastal::navigation::detail {
// Positive-edge Dijkstra queue. A packed unsigned key preserves the original
// pair<int32_t,int> ordering for nonnegative distances and cell indices.
// For the supported <=512x512 grid, even a simple path of V-1 edges at 1414
// costs at most 370,670,202, safely inside int32_t. The cast precedes the shift.
class PackedMinHeap4 {
    std::vector<std::uint64_t> data_;
public:
    bool empty() const noexcept {return data_.empty();}
    std::pair<std::int32_t,int> top() const {
        assert(!data_.empty());const auto key=data_.front();
        return {std::int32_t(key>>32),int(std::uint32_t(key))};
    }
    void emplace(std::int32_t distance,int cell){
        assert(distance>=0&&cell>=0);
        const auto key=(std::uint64_t(std::uint32_t(distance))<<32)|std::uint32_t(cell);
        std::size_t index=data_.size();data_.push_back(key);
        while(index){
            const auto parent=(index-1)/4;
            if(data_[parent]<=key)break;
            data_[index]=data_[parent];index=parent;
        }
        data_[index]=key;
    }
    void pop(){
        assert(!data_.empty());const auto key=data_.back();data_.pop_back();
        if(data_.empty())return;
        std::size_t index=0;
        while(index*4+1<data_.size()){
            const auto first=index*4+1,last=std::min(data_.size(),first+4);
            auto smallest=first;
            for(auto child=first+1;child<last;++child)if(data_[child]<data_[smallest])smallest=child;
            if(key<=data_[smallest])break;
            data_[index]=data_[smallest];index=smallest;
        }
        data_[index]=key;
    }
};
}
