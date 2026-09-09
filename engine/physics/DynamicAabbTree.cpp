#include "DynamicAabbTree.h"
#include <cmath>

namespace whimsical {
namespace {
constexpr float Margin = .1f;
PhysicsBounds merged(PhysicsBounds a, PhysicsBounds b) {
    return {glm::min(a.min, b.min), glm::max(a.max, b.max)};
}
PhysicsBounds expanded(PhysicsBounds b, float margin) {
    return {b.min - margin, b.max + margin};
}
bool contains(PhysicsBounds a, PhysicsBounds b) {
    return glm::all(glm::lessThanEqual(a.min, b.min)) && glm::all(glm::greaterThanEqual(a.max, b.max));
}
float area(PhysicsBounds b) {
    const auto d = b.max - b.min;
    return 2 * (d.x * d.y + d.y * d.z + d.z * d.x);
}
} // namespace

int DynamicAabbTree::allocate() {
    int index;
    if (free_ == -1) {
        index = int(nodes_.size());
        nodes_.push_back({});
    } else {
        index = free_;
        free_ = nodes_[index].parent;
        nodes_[index] = {};
    }
    nodes_[index].height = 0;
    return index;
}
void DynamicAabbTree::release(int index) {
    nodes_[index].height = -1;
    nodes_[index].parent = free_;
    free_ = index;
}
int DynamicAabbTree::insert(uint32_t payload, PhysicsBounds bounds) {
    const int leaf = allocate();
    nodes_[leaf].payload = payload;
    nodes_[leaf].bounds = expanded(bounds, Margin);
    insertLeaf(leaf);
    ++statistics_.leaves;
    return leaf;
}
void DynamicAabbTree::remove(int leaf) {
    detachLeaf(leaf);
    release(leaf);
    --statistics_.leaves;
}
void DynamicAabbTree::update(int leaf, PhysicsBounds bounds) {
    // Small movements remain inside the fat bounds. Shrinking large shapes must also tighten the leaf.
    if (contains(nodes_[leaf].bounds, bounds) && contains(expanded(bounds, Margin * 4), nodes_[leaf].bounds))
        return;
    detachLeaf(leaf);
    nodes_[leaf].bounds = expanded(bounds, Margin);
    insertLeaf(leaf);
    ++statistics_.reinsertions;
}
void DynamicAabbTree::refit(int index) {
    auto& n = nodes_[index];
    n.bounds = merged(nodes_[n.left].bounds, nodes_[n.right].bounds);
    n.height = 1 + std::max(nodes_[n.left].height, nodes_[n.right].height);
}
int DynamicAabbTree::balance(int index) {
    auto& a = nodes_[index];
    if (a.leaf() || a.height < 2)
        return index;
    const int left = a.left, right = a.right;
    const int difference = nodes_[right].height - nodes_[left].height;
    if (std::abs(difference) <= 1)
        return index;
    // Promote the heavy child, retaining its taller grandchild on the outside.
    const bool rightHeavy = difference > 1;
    const int promoted = rightHeavy ? right : left;
    auto& b = nodes_[promoted];
    const int child1 = b.left, child2 = b.right;
    b.parent = a.parent;
    if (a.parent == -1)
        root_ = promoted;
    else {
        auto& parent = nodes_[a.parent];
        (parent.left == index ? parent.left : parent.right) = promoted;
    }
    a.parent = promoted;
    const bool firstTaller = nodes_[child1].height > nodes_[child2].height;
    const int outside = firstTaller ? child1 : child2;
    const int inside = firstTaller ? child2 : child1;
    if (rightHeavy) {
        b.left = index;
        b.right = outside;
        a.right = inside;
    } else {
        b.left = outside;
        b.right = index;
        a.left = inside;
    }
    nodes_[outside].parent = promoted;
    nodes_[inside].parent = index;
    refit(index);
    refit(promoted);
    return promoted;
}
void DynamicAabbTree::fixAncestors(int index) {
    while (index != -1) {
        refit(index);
        index = balance(index);
        index = nodes_[index].parent;
    }
}
void DynamicAabbTree::insertLeaf(int leaf) {
    if (root_ == -1) {
        root_ = leaf;
        nodes_[leaf].parent = -1;
        return;
    }
    const auto bounds = nodes_[leaf].bounds;
    int sibling = root_;
    while (!nodes_[sibling].leaf()) {
        const auto& n = nodes_[sibling];
        const float combined = area(merged(n.bounds, bounds));
        const float inherited = 2 * (combined - area(n.bounds));
        auto cost = [&](int child) {
            const auto& c = nodes_[child];
            return area(merged(c.bounds, bounds)) - (c.leaf() ? 0.f : area(c.bounds)) + inherited;
        };
        const float leftCost = cost(n.left), rightCost = cost(n.right);
        if (2 * combined < leftCost && 2 * combined < rightCost)
            break;
        sibling = leftCost < rightCost ? n.left : n.right;
    }
    const int previousParent = nodes_[sibling].parent;
    const int parent = allocate();
    nodes_[parent].parent = previousParent;
    nodes_[parent].left = sibling;
    nodes_[parent].right = leaf;
    nodes_[sibling].parent = parent;
    nodes_[leaf].parent = parent;
    if (previousParent == -1)
        root_ = parent;
    else {
        auto& p = nodes_[previousParent];
        (p.left == sibling ? p.left : p.right) = parent;
    }
    fixAncestors(parent);
}
void DynamicAabbTree::detachLeaf(int leaf) {
    if (root_ == leaf) {
        root_ = -1;
        nodes_[leaf].parent = -1;
        return;
    }
    const int parent = nodes_[leaf].parent;
    const int grandparent = nodes_[parent].parent;
    const int sibling = nodes_[parent].left == leaf ? nodes_[parent].right : nodes_[parent].left;
    if (grandparent == -1)
        root_ = sibling;
    else {
        auto& g = nodes_[grandparent];
        (g.left == parent ? g.left : g.right) = sibling;
    }
    nodes_[sibling].parent = grandparent;
    nodes_[leaf].parent = -1;
    release(parent);
    fixAncestors(grandparent);
}
int DynamicAabbTree::build(std::vector<int>& leaves, size_t begin, size_t end) {
    if (end - begin == 1)
        return leaves[begin];
    PhysicsBounds centers;
    centers.min = centers.max = (nodes_[leaves[begin]].bounds.min + nodes_[leaves[begin]].bounds.max) * .5f;
    for (size_t i = begin + 1; i < end; ++i) {
        const auto& b = nodes_[leaves[i]].bounds;
        auto c = (b.min + b.max) * .5f;
        centers.min = glm::min(centers.min, c);
        centers.max = glm::max(centers.max, c);
    }
    auto extent = centers.max - centers.min;
    int axis = extent.x > extent.y ? 0 : 1;
    if (extent.z > extent[axis])
        axis = 2;
    const auto middle = begin + (end - begin) / 2;
    std::nth_element(leaves.begin() + begin, leaves.begin() + middle, leaves.begin() + end,
                     [&](int a, int b) {
                         const auto& x = nodes_[a];
                         const auto& y = nodes_[b];
                         const float cx = x.bounds.min[axis] + x.bounds.max[axis];
                         const float cy = y.bounds.min[axis] + y.bounds.max[axis];
                         return cx == cy ? x.payload < y.payload : cx < cy;
                     });
    const int left = build(leaves, begin, middle), right = build(leaves, middle, end);
    const int parent = allocate();
    nodes_[parent].left = left;
    nodes_[parent].right = right;
    nodes_[left].parent = nodes_[right].parent = parent;
    refit(parent);
    return parent;
}
void DynamicAabbTree::rebuild() {
    std::vector<int> leaves;
    for (int i = 0; i < int(nodes_.size()); ++i) {
        if (nodes_[i].height < 0)
            continue;
        if (nodes_[i].leaf()) {
            nodes_[i].parent = -1;
            leaves.push_back(i);
        } else
            release(i);
    }
    root_ = leaves.empty() ? -1 : build(leaves, 0, leaves.size());
    ++statistics_.rebuilds;
}
BroadphaseStatistics DynamicAabbTree::statistics() const {
    auto result = statistics_;
    result.height = root_ == -1 ? 0 : nodes_[root_].height;
    return result;
}
std::vector<uint32_t> DynamicAabbTree::overlap(PhysicsBounds bounds, size_t* visited) const {
    return query(
        [&](PhysicsBounds b) {
            return glm::all(glm::lessThanEqual(b.min, bounds.max)) &&
                   glm::all(glm::greaterThanEqual(b.max, bounds.min));
        },
        visited);
}
std::vector<uint32_t> DynamicAabbTree::raycast(vec3 o, vec3 d, float distance, size_t* visited) const {
    return query(
        [&](PhysicsBounds b) {
            float near = 0, far = distance;
            for (int axis = 0; axis < 3; ++axis) {
                if (std::abs(d[axis]) < 1e-5f) {
                    if (o[axis] < b.min[axis] || o[axis] > b.max[axis])
                        return false;
                } else {
                    float a = (b.min[axis] - o[axis]) / d[axis], z = (b.max[axis] - o[axis]) / d[axis];
                    if (a > z)
                        std::swap(a, z);
                    near = std::max(near, a);
                    far = std::min(far, z);
                    if (near > far)
                        return false;
                }
            }
            return true;
        },
        visited);
}
} // namespace whimsical
