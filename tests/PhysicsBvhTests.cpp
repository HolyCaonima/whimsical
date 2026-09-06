#include "physics/PhysicsScene.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>
using namespace afterlight;
namespace {
void check(bool value, const char* why) {
    if (!value)
        throw std::runtime_error(why);
}
bool intersects(PhysicsBounds a, PhysicsBounds b) {
    return glm::all(glm::lessThanEqual(a.min, b.max)) && glm::all(glm::greaterThanEqual(a.max, b.min));
}
void treeTests() {
    DynamicAabbTree tree;
    size_t visits = 99;
    check(tree.overlap({vec3(-1), vec3(1)}, &visits).empty() && visits == 0, "Empty BVH query");
    tree.rebuild();
    std::vector<int> leaves;
    constexpr int Count = 4096;
    for (int i = 0; i < Count; ++i) {
        const vec3 p(float(i) * 4, 0, 0);
        leaves.push_back(tree.insert(uint32_t(i), {p - .5f, p + .5f}));
    }
    check(tree.statistics().leaves == Count && tree.statistics().height < 32,
          "Sorted incremental insertions must not create a linear tree");
    const auto local = tree.overlap({{-.2f, -.2f, -.2f}, {.2f, .2f, .2f}}, &visits);
    check(local == std::vector<uint32_t>{0} && visits < 128, "Local overlap must prune distant subtrees");
    auto ray = tree.raycast({0, 3, 0}, {0, -1, 0}, 5, &visits);
    check(ray == std::vector<uint32_t>{0} && visits < 128, "Ray traversal must prune distant subtrees");
    std::cout << "4096 ordered bodies: height=" << tree.statistics().height << ", local ray nodes=" << visits
              << '\n';
    tree.update(leaves[0], {vec3(-.48f), vec3(.52f)});
    check(tree.statistics().reinsertions == 0, "Small motion should reuse a fat leaf");
    tree.update(leaves[0], {{-20, -1, -1}, {-18, 1, 1}});
    check(tree.statistics().reinsertions == 1 && tree.overlap({vec3(-.2f), vec3(.2f)}).empty(),
          "Teleport must remove the old bounds");
    tree.rebuild();
    check(tree.statistics().height == 12 && tree.statistics().rebuilds == 2,
          "Bulk rebuild should create a balanced tree");
    tree.update(leaves[0], {vec3(-.5f), vec3(.5f)});
    check(tree.overlap({vec3(-.2f), vec3(.2f)}) == std::vector<uint32_t>{0},
          "Leaf handles must survive bulk rebuild");
    for (int i = 0; i < Count; i += 2)
        tree.remove(leaves[i]);
    tree.rebuild();
    for (int i = 1; i < Count; i += 2)
        tree.remove(leaves[i]);
    check(tree.statistics().leaves == 0 && tree.raycast({0, 0, 0}, {1, 0, 0}, 1e6f).empty(),
          "Removing every leaf must empty the tree");
    auto leaf = tree.insert(42, {vec3(-10), vec3(10)});
    tree.update(leaf, {vec3(-.1f), vec3(.1f)});
    check(tree.overlap({vec3(5), vec3(6)}).empty(), "Shrinking a large shape must tighten fat bounds");
    tree.remove(leaf);

    // Independent brute-force bounds oracle after random relocations, removals and rebuilds.
    std::mt19937 rng(71519);
    std::uniform_real_distribution<float> pos(-30, 30);
    std::vector<PhysicsBounds> boxes(256);
    leaves.resize(boxes.size());
    for (size_t i = 0; i < boxes.size(); ++i) {
        vec3 p(pos(rng), pos(rng), pos(rng));
        boxes[i] = {p - 1.f, p + 1.f};
        leaves[i] = tree.insert(uint32_t(i), boxes[i]);
    }
    for (int round = 0; round < 1500; ++round) {
        const auto index = rng() % boxes.size();
        vec3 p(pos(rng), pos(rng), pos(rng));
        boxes[index] = {p - 1.f, p + 1.f};
        if (round % 3 == 0) {
            tree.remove(leaves[index]);
            leaves[index] = tree.insert(uint32_t(index), boxes[index]);
        } else
            tree.update(leaves[index], boxes[index]);
        if (round % 100 == 0)
            tree.rebuild();
        PhysicsBounds query{p - 3.f, p + 3.f};
        const auto candidates = tree.overlap(query);
        for (size_t i = 0; i < boxes.size(); ++i)
            if (intersects(boxes[i], query))
                check(std::binary_search(candidates.begin(), candidates.end(), uint32_t(i)),
                      "Incremental BVH missed a brute-force bounds intersection");
    }
}
bool close(vec3 a, vec3 b) {
    return glm::distance(a, b) < 1e-4f;
}
void equalHit(const PhysicsHit& a, const PhysicsHit& b) {
    check(a.body == b.body && a.owner == b.owner && std::abs(a.distance - b.distance) < 1e-4f &&
              std::abs(a.fraction - b.fraction) < 1e-4f && close(a.position, b.position) &&
              close(a.normal, b.normal),
          "BVH result differs from linear per-body reference");
}
void equalHit(const std::optional<PhysicsHit>& a, const std::optional<PhysicsHit>& b) {
    check(bool(a) == bool(b), "BVH query missed or invented a hit");
    if (a)
        equalHit(*a, *b);
}
void sceneTests() {
    PhysicsScene scene;
    std::vector<BodyHandle> handles;
    std::mt19937 rng(39421);
    std::uniform_real_distribution<float> value(-1, 1);
    auto position = [&] { return vec3(value(rng) * 12, value(rng) * 3, value(rng) * 12); };
    auto shape = [&] {
        return rng() % 2 ? ColliderShape::box({.3f + std::abs(value(rng)), .7f, .5f})
                         : ColliderShape::capsule(.3f, 1.4f);
    };
    for (uint32_t i = 0; i < 96; ++i) {
        PhysicsBody b;
        b.owner = i + 1;
        b.pose.position = position();
        b.shape = shape();
        handles.push_back(scene.create(b));
    }
    scene.rebuildBroadphase();
    const auto initial = scene.broadphaseStatistics();
    scene.setPose(handles[0], scene.body(handles[0]).pose);
    check(scene.broadphaseStatistics().reinsertions == initial.reinsertions,
          "Unchanged pose must not edit BVH");
    for (int round = 0; round < 240; ++round) {
        const auto index = rng() % handles.size();
        auto h = handles[index];
        switch (round % 7) {
        case 0:
            scene.setPose(h, {position(), glm::angleAxis(value(rng) * Pi, glm::normalize(vec3(1, 2, 3)))});
            break;
        case 1:
            scene.setShape(h, shape());
            break;
        case 2:
            scene.setEnabled(h, !scene.body(h).enabled);
            break;
        case 3:
            scene.setProperties(h, 1u << (rng() % 3), rng() % 2, rng() % 2, rng() % 2);
            break;
        case 4: {
            auto b = scene.body(h);
            scene.destroy(h);
            b.pose.position = position();
            b.enabled = true;
            handles[index] = scene.create(b);
            check(!scene.contains(h), "Deleted handles must remain stale after BVH leaf reuse");
            break;
        }
        case 5:
            scene.rebuildBroadphase();
            break;
        case 6:
            scene.setMotion(h, BodyMotion::Kinematic);
            break;
        }
        QueryFilter filter;
        filter.mask = round % 4 ? CollisionLayer::All : CollisionLayer::World;
        filter.ignoreOwner = round % 5 ? 0 : 1;
        filter.blockingOnly = round % 3 == 0;
        filter.walkableOnly = round % 11 == 0;
        filter.pickableOnly = round % 7 == 0;
        const vec3 origin = position();
        const vec3 direction = glm::normalize(position() - origin);
        CapsuleQuery capsule{position(), .4f, 1.8f};
        vec3 delta = position() - capsule.center;
        if (round % 13 == 0)
            delta = vec3(0);
        std::optional<PhysicsHit> rayReference, sweepReference;
        std::vector<PhysicsHit> overlapReference;
        auto ordered = handles;
        std::sort(ordered.begin(), ordered.end(), [](auto a, auto b) { return a.slot < b.slot; });
        // Test-only linear oracle: test each body in isolation, using unchanged narrow phase.
        // No linear fallback or validation mode is added to production queries.
        for (auto body : ordered) {
            PhysicsScene single;
            single.create(scene.body(body));
            auto ray = single.raycast(origin, direction, 35, filter);
            if (ray && (!rayReference || ray->distance < rayReference->distance)) {
                ray->body = body;
                rayReference = ray;
            }
            auto sweep = single.sweepCapsule(capsule, delta, filter);
            if (sweep && (!sweepReference || sweep->fraction <= sweepReference->fraction)) {
                sweep->body = body;
                sweepReference = sweep;
            }
            for (auto hit : single.overlapCapsule(capsule, filter)) {
                hit.body = body;
                overlapReference.push_back(hit);
            }
        }
        equalHit(scene.raycast(origin, direction, 35, filter), rayReference);
        equalHit(scene.sweepCapsule(capsule, delta, filter), sweepReference);
        auto overlaps = scene.overlapCapsule(capsule, filter);
        check(overlaps.size() == overlapReference.size(), "Overlap count differs from linear reference");
        for (size_t i = 0; i < overlaps.size(); ++i)
            equalHit(overlaps[i], overlapReference[i]);
        const auto enabled = scene.bodies().size();
        check(scene.broadphaseStatistics().leaves == enabled, "Only enabled bodies belong in the BVH");
    }
    // Disabled objects still accept edits; reenabling inserts their latest bounds.
    const auto h = handles[0];
    scene.setEnabled(h, false);
    scene.setPose(h, {{100, 1, 0}, quat(1, 0, 0, 0)});
    scene.setShape(h, ColliderShape::box(vec3(1)));
    scene.setProperties(h, CollisionLayer::World, false, false, false);
    scene.setEnabled(h, true);
    auto hit = scene.raycast({100, 5, 0}, {0, -1, 0}, 10);
    check(hit && hit->body == h, "Unfiltered queries must retain decoration bodies after reenabling");
    check(!scene.raycast({100, 5, 0}, {0, -1, 0}, 10, {CollisionLayer::All, 0, true}),
          "Property-only edits must immediately affect filtering");
    auto inside = scene.raycast({100, 1, 0}, {1, 0, 0}, 0);
    check(inside && inside->distance == 0, "Zero-length ray starting inside a leaf");
    for (auto body : handles)
        scene.destroy(body);
    scene.rebuildBroadphase();
    check(scene.broadphaseStatistics().leaves == 0 && scene.size() == 0,
          "Scene clear must release BVH leaves");
}
} // namespace
int main() {
    try {
        treeTests();
        sceneTests();
        std::cout << "PASS: BVH bulk build, incremental lifecycle, query pruning and randomized linear "
                     "equivalence\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
