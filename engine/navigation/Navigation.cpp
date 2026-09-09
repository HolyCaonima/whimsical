#include "Navigation.h"
#include <queue>
#include <algorithm>
#include <limits>
#include <stdexcept>
namespace whimsical {
namespace {
QueryFilter blockers(const NavigationAgent& a) {
    QueryFilter f;
    f.mask = CollisionLayer::World | CollisionLayer::Character;
    f.ignoreOwner = a.owner;
    f.blockingOnly = true;
    return f;
}
bool supported(const PhysicsScene& scene, vec3 feet, const NavigationAgent& a, const NavigationSettings& s) {
    if (feet.x < s.min.x + a.radius || feet.x > s.max.x - a.radius || feet.z < s.min.z + a.radius ||
        feet.z > s.max.z - a.radius)
        return false;
    for (vec2 offset : {vec2(0), vec2(a.radius * .7f, 0), vec2(-a.radius * .7f, 0), vec2(0, a.radius * .7f),
                        vec2(0, -a.radius * .7f)}) {
        auto p = Navigation::ground(scene, feet + vec3(offset.x, 0, offset.y), s);
        if (!p || std::abs(p->y - feet.y) > s.planeTolerance)
            return false;
    }
    return true;
}
} // namespace
std::optional<vec3> Navigation::ground(const PhysicsScene& scene, vec3 p, const NavigationSettings& s) {
    QueryFilter filter;
    filter.walkableOnly = true;
    filter.mask = CollisionLayer::World;
    auto hit = scene.raycast({p.x, s.max.y + .1f, p.z}, vec3(0, -1, 0), s.max.y - s.min.y + .2f, filter);
    if (!hit || hit->normal.y < .95f)
        return {};
    return hit->position;
}
bool Navigation::canStand(const PhysicsScene& scene, vec3 feet, const NavigationAgent& a,
                          const NavigationSettings& s) {
    return supported(scene, feet, a, s) &&
           scene.overlapCapsule({feet + vec3(0, a.height * .5f, 0), a.radius, a.height}, blockers(a)).empty();
}
bool Navigation::lineClear(const PhysicsScene& scene, vec3 start, vec3 end, const NavigationAgent& a,
                           const NavigationSettings& s) {
    if (!canStand(scene, start, a, s) || !canStand(scene, end, a, s))
        return false;
    if (scene.sweepCapsule({start + vec3(0, a.height * .5f, 0), a.radius, a.height}, end - start,
                           blockers(a)))
        return false;
    int steps = std::max(1, int(std::ceil(glm::distance(start, end) / std::min(.1f, a.radius * .5f))));
    for (int i = 1; i < steps; ++i)
        if (!supported(scene, glm::mix(start, end, float(i) / steps), a, s))
            return false;
    return true;
}
std::vector<vec3> Navigation::findPath(const PhysicsScene& scene, vec3 start, vec3 end,
                                       const NavigationAgent& agent, const NavigationSettings& settings) {
    if (!std::isfinite(settings.cellSize) || settings.cellSize < .05f || settings.max.x <= settings.min.x ||
        settings.max.z <= settings.min.z || settings.max.y <= settings.min.y)
        throw std::invalid_argument("Invalid navigation bounds/cell size");
    auto startGround = ground(scene, start, settings), endGround = ground(scene, end, settings);
    if (!startGround || !endGround || std::abs(startGround->y - endGround->y) > settings.planeTolerance)
        return {};
    start = *startGround;
    end = *endGround;
    if (!canStand(scene, start, agent, settings) || !canStand(scene, end, agent, settings))
        return {};
    if (lineClear(scene, start, end, agent, settings))
        return {end};
    const float cell = settings.cellSize;
    int W = int(std::ceil((settings.max.x - settings.min.x) / cell)),
        H = int(std::ceil((settings.max.z - settings.min.z) / cell));
    if (W > 1024 || H > 1024)
        throw std::invalid_argument("Navigation grid exceeds supported size");
    auto coord = [&](vec3 p) {
        return glm::ivec2(std::clamp(int((p.x - settings.min.x) / cell), 0, W - 1),
                          std::clamp(int((p.z - settings.min.z) / cell), 0, H - 1));
    };
    auto point = [&](int i) {
        return vec3(settings.min.x + (i % W + .5f) * cell, start.y, settings.min.z + (i / W + .5f) * cell);
    };
    auto a = coord(start), b = coord(end);
    int source = a.y * W + a.x, target = b.y * W + b.x;
    std::vector<float> cost(W * H, std::numeric_limits<float>::infinity());
    std::vector<int> parent(W * H, -1);
    std::vector<uint8_t> closed(W * H), valid(W * H, 2);
    auto pass = [&](int i) {
        if (valid[i] == 2)
            valid[i] = uint8_t(canStand(scene, point(i), agent, settings));
        return valid[i] != 0;
    };
    using Item = std::pair<float, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
    cost[source] = 0;
    open.push({0.f, source});
    while (!open.empty()) {
        int i = open.top().second;
        open.pop();
        if (closed[i])
            continue;
        closed[i] = 1;
        if (i == target)
            break;
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x) {
                if (!x && !y)
                    continue;
                int nx = i % W + x, ny = i / W + y;
                if (nx < 0 || ny < 0 || nx >= W || ny >= H)
                    continue;
                int j = ny * W + nx;
                if (!pass(j) || closed[j])
                    continue;
                if (x && y && (!pass((i / W) * W + nx) || !pass(ny * W + i % W)))
                    continue;
                if (!lineClear(scene, i == source ? start : point(i), j == target ? end : point(j), agent,
                               settings))
                    continue;
                float candidate = cost[i] + (x && y ? 1.414214f : 1.f);
                if (candidate < cost[j]) {
                    cost[j] = candidate;
                    parent[j] = i;
                    open.push({candidate + glm::length(vec2(nx - b.x, ny - b.y)), j});
                }
            }
    }
    if (source != target && parent[target] < 0)
        return {};
    std::vector<vec3> raw{end};
    for (int i = target; i != source && i >= 0; i = parent[i])
        raw.push_back(point(i));
    raw.push_back(start);
    std::reverse(raw.begin(), raw.end());
    std::vector<vec3> result;
    for (size_t anchor = 0; anchor + 1 < raw.size();) {
        size_t far = anchor + 1;
        while (far + 1 < raw.size() && lineClear(scene, raw[anchor], raw[far + 1], agent, settings))
            ++far;
        if (!lineClear(scene, raw[anchor], raw[far], agent, settings))
            return {};
        result.push_back(raw[far]);
        anchor = far;
    }
    return result;
}
} // namespace whimsical
