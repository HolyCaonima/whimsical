#pragma once
#include "core/Math.h"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace whimsical {
struct PhysicsBounds {
    vec3 min{0}, max{0};
};
struct BroadphaseStatistics {
    size_t leaves = 0;
    int height = 0;
    uint64_t reinsertions = 0, rebuilds = 0;
};

// Owns only spatial bounds and opaque payloads. PhysicsScene owns bodies and filtering.
// Leaf indices remain stable across rotations and bulk rebuilds.
class DynamicAabbTree {
    struct Node {
        PhysicsBounds bounds;
        int parent = -1, left = -1, right = -1, height = -1;
        uint32_t payload = 0;
        bool leaf() const {
            return left == -1;
        }
    };
    std::vector<Node> nodes_;
    int root_ = -1, free_ = -1;
    BroadphaseStatistics statistics_;
    int allocate();
    void release(int node);
    void insertLeaf(int leaf);
    void detachLeaf(int leaf);
    void refit(int node);
    int balance(int node);
    void fixAncestors(int node);
    int build(std::vector<int>& leaves, size_t begin, size_t end);

    template <class Predicate> std::vector<uint32_t> query(Predicate intersects, size_t* visited) const {
        if (visited)
            *visited = 0;
        std::vector<uint32_t> result;
        std::vector<int> stack;
        if (root_ != -1)
            stack.push_back(root_);
        while (!stack.empty()) {
            const auto index = stack.back();
            stack.pop_back();
            const auto& node = nodes_[index];
            if (visited)
                ++*visited;
            if (!intersects(node.bounds))
                continue;
            if (node.leaf())
                result.push_back(node.payload);
            else {
                stack.push_back(node.left);
                stack.push_back(node.right);
            }
        }
        // Preserve PhysicsScene's slot-order tie breaking, independently of tree topology.
        std::sort(result.begin(), result.end());
        return result;
    }

  public:
    int insert(uint32_t payload, PhysicsBounds bounds);
    void remove(int leaf);
    void update(int leaf, PhysicsBounds bounds);
    // Balanced spatial build for bulk imports; normal mutations never call this.
    void rebuild();
    BroadphaseStatistics statistics() const;
    std::vector<uint32_t> overlap(PhysicsBounds bounds, size_t* visited = nullptr) const;
    // Direction must be normalized. Uses the same near-parallel tolerance as narrow phase.
    std::vector<uint32_t> raycast(vec3 origin, vec3 direction, float distance,
                                  size_t* visited = nullptr) const;
};
} // namespace whimsical
