#pragma once
#include <cstdint>
#include <map>
namespace whimsical {
// Stable geometry ranges. Released holes are coalesced and reused without moving
// live meshes or invalidating their BLAS. GPU buffers grow only at the high-water mark.
class RangeAllocator {
    std::map<uint32_t, uint32_t> free_;
    uint32_t size_ = 0;

  public:
    uint32_t size() const {
        return size_;
    }
    uint32_t allocate(uint32_t count) {
        for (auto p = free_.begin(); p != free_.end(); ++p)
            if (p->second >= count) {
                auto first = p->first, left = p->second - count;
                free_.erase(p);
                if (left)
                    free_.emplace(first + count, left);
                return first;
            }
        auto first = size_;
        size_ += count;
        return first;
    }
    void release(uint32_t first, uint32_t count) {
        auto next = free_.lower_bound(first);
        if (next != free_.begin()) {
            auto prev = std::prev(next);
            if (prev->first + prev->second == first) {
                first = prev->first;
                count += prev->second;
                free_.erase(prev);
            }
        }
        if (next != free_.end() && first + count == next->first) {
            count += next->second;
            free_.erase(next);
        }
        if (first + count == size_)
            size_ = first;
        else
            free_.emplace(first, count);
    }
};
} // namespace whimsical
