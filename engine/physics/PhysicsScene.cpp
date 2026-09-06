#include "PhysicsScene.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
namespace afterlight {
namespace {
constexpr float Epsilon = 1e-5f, Skin = .002f;
bool finite(vec3 p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
void validate(const ColliderShape& s) {
    if (!finite(s.halfExtents) || glm::any(glm::lessThanEqual(s.halfExtents, vec3(0))) ||
        !std::isfinite(s.radius) || !std::isfinite(s.halfSegment) || s.radius <= 0 || s.halfSegment < 0)
        throw std::invalid_argument("Invalid collider dimensions");
}
void validate(const PhysicsPose& p) {
    float q = glm::dot(p.rotation, p.rotation);
    if (!finite(p.position) || !std::isfinite(q) || std::abs(q - 1) > .001f)
        throw std::invalid_argument("Physics pose requires a finite position and unit quaternion");
}
void validate(const CapsuleQuery& c) {
    if (!finite(c.center) || !std::isfinite(c.height) || !std::isfinite(c.radius) || c.radius <= 0 ||
        c.height < 2 * c.radius)
        throw std::invalid_argument("Invalid query capsule");
}
PhysicsBounds bodyBounds(const PhysicsBody& b) {
    vec3 extent;
    if (b.shape.type == ColliderType::Capsule)
        extent = glm::abs(b.pose.rotation * vec3(0, b.shape.halfSegment, 0)) + b.shape.radius;
    else {
        auto m = glm::mat3_cast(b.pose.rotation);
        extent = glm::abs(m[0]) * b.shape.halfExtents.x + glm::abs(m[1]) * b.shape.halfExtents.y +
                 glm::abs(m[2]) * b.shape.halfExtents.z;
    }
    return {b.pose.position - extent, b.pose.position + extent};
}
bool intersects(PhysicsBounds a, PhysicsBounds b) {
    return glm::all(glm::lessThanEqual(a.min, b.max)) && glm::all(glm::greaterThanEqual(a.max, b.min));
}
PhysicsBounds capsuleBounds(CapsuleQuery c, vec3 delta = vec3(0)) {
    vec3 e(c.radius, c.height * .5f, c.radius);
    return {glm::min(c.center, c.center + delta) - e - Skin, glm::max(c.center, c.center + delta) + e + Skin};
}
// Minimize squared segment/AABB distance exactly over the intervals where clamping is affine.
void segmentBox(vec3 a, vec3 b, vec3 h, vec3& onSegment, vec3& onBox) {
    vec3 d = b - a;
    std::array<float, 8> cuts{};
    int count = 2;
    cuts[0] = 0;
    cuts[1] = 1;
    for (int axis = 0; axis < 3; ++axis)
        if (std::abs(d[axis]) > Epsilon)
            for (float sign : {-1.f, 1.f}) {
                float t = (sign * h[axis] - a[axis]) / d[axis];
                if (t > 0 && t < 1)
                    cuts[count++] = t;
            }
    std::sort(cuts.begin(), cuts.begin() + count);
    float best = std::numeric_limits<float>::infinity();
    auto sample = [&](float t) {
        vec3 p = a + d * t, q = glm::clamp(p, -h, h);
        float distance2 = glm::dot(p - q, p - q);
        if (distance2 < best) {
            best = distance2;
            onSegment = p;
            onBox = q;
        }
    };
    for (int i = 0; i + 1 < count; ++i) {
        float mid = (cuts[i] + cuts[i + 1]) * .5f, linear = 0, quadratic = 0;
        for (int axis = 0; axis < 3; ++axis) {
            float p = a[axis] + d[axis] * mid;
            if (std::abs(p) > h[axis]) {
                float boundary = p > 0 ? h[axis] : -h[axis];
                linear += d[axis] * (a[axis] - boundary);
                quadratic += d[axis] * d[axis];
            }
        }
        sample(cuts[i]);
        sample(cuts[i + 1]);
        if (quadratic > 0)
            sample(std::clamp(-linear / quadratic, cuts[i], cuts[i + 1]));
    }
}
void segmentSegment(vec3 a, vec3 b, vec3 c, vec3 d, vec3& p, vec3& q) {
    vec3 u = b - a, v = d - c, w = a - c;
    float uu = glm::dot(u, u), vv = glm::dot(v, v), uv = glm::dot(u, v), uw = glm::dot(u, w),
          vw = glm::dot(v, w), s = 0, t = 0;
    if (uu < Epsilon && vv < Epsilon) {
        p = a;
        q = c;
        return;
    }
    if (uu < Epsilon)
        t = std::clamp(vw / vv, 0.f, 1.f);
    else {
        if (vv < Epsilon)
            s = std::clamp(-uw / uu, 0.f, 1.f);
        else {
            float den = uu * vv - uv * uv;
            if (den > Epsilon)
                s = std::clamp((uv * vw - uw * vv) / den, 0.f, 1.f);
            t = (uv * s + vw) / vv;
            if (t < 0) {
                t = 0;
                s = std::clamp(-uw / uu, 0.f, 1.f);
            }
            if (t > 1) {
                t = 1;
                s = std::clamp((uv - uw) / uu, 0.f, 1.f);
            }
        }
    }
    p = a + s * u;
    q = c + t * v;
}
struct Separation {
    float distance;
    vec3 normal, point;
};
Separation separation(CapsuleQuery c, const PhysicsBody& b) {
    vec3 axis(0, c.height * .5f - c.radius, 0), p, q;
    if (b.shape.type == ColliderType::Capsule) {
        vec3 otherAxis = b.pose.rotation * vec3(0, b.shape.halfSegment, 0);
        segmentSegment(c.center - axis, c.center + axis, b.pose.position - otherAxis,
                       b.pose.position + otherAxis, p, q);
        float length = glm::length(p - q);
        vec3 normal = length > Epsilon ? (p - q) / length : vec3(1, 0, 0);
        return {length - c.radius - b.shape.radius, normal, q + normal * b.shape.radius};
    }
    quat inverse = glm::conjugate(b.pose.rotation);
    segmentBox(inverse * (c.center - axis - b.pose.position), inverse * (c.center + axis - b.pose.position),
               b.shape.halfExtents, p, q);
    float length = glm::length(p - q);
    if (length > Epsilon)
        return {length - c.radius, b.pose.rotation * ((p - q) / length),
                b.pose.position + b.pose.rotation * q};
    vec3 toFace = b.shape.halfExtents - glm::abs(p);
    int dim = toFace.x < toFace.y ? 0 : 1;
    if (toFace.z < toFace[dim])
        dim = 2;
    vec3 normal(0);
    normal[dim] = p[dim] < 0 ? -1.f : 1.f;
    q = p;
    q[dim] = normal[dim] * b.shape.halfExtents[dim];
    return {-c.radius - toFace[dim], b.pose.rotation * normal, b.pose.position + b.pose.rotation * q};
}
float shapeRadius(const ColliderShape& shape) {
    return shape.type == ColliderType::Box ? glm::length(shape.halfExtents)
                                          : shape.radius + shape.halfSegment;
}
vec3 supportPoint(const PhysicsBody& b, vec3 direction) {
    vec3 local = glm::conjugate(b.pose.rotation) * direction;
    vec3 point;
    if (b.shape.type == ColliderType::Box)
        point = glm::sign(local) * b.shape.halfExtents;
    else
        point = glm::normalize(local) * b.shape.radius +
                vec3(0, glm::sign(local.y) * b.shape.halfSegment, 0);
    return b.pose.position + b.pose.rotation * point;
}
Separation separation(const ShapeQuery& a, const PhysicsBody& b) {
    if (a.shape.type == ColliderType::Capsule) {
        // Reuse the exact capsule distance query in its own upright coordinate frame.
        auto inverse = glm::conjugate(a.pose.rotation);
        PhysicsBody local = b;
        local.pose = {inverse * (b.pose.position - a.pose.position), inverse * b.pose.rotation};
        auto hit = separation(CapsuleQuery{vec3(0), a.shape.radius, a.shape.height()}, local);
        return {hit.distance, a.pose.rotation * hit.normal,
                a.pose.position + a.pose.rotation * hit.point};
    }
    if (b.shape.type == ColliderType::Capsule) {
        PhysicsBody other;
        other.shape = a.shape;
        other.pose = a.pose;
        auto hit = separation(ShapeQuery{b.shape, b.pose}, other);
        return {hit.distance, -hit.normal, supportPoint(b, -hit.normal)};
    }
    // OBB separating axes give signed overlap and a conservative distance bound for CCD.
    auto axesA = glm::mat3_cast(a.pose.rotation), axesB = glm::mat3_cast(b.pose.rotation);
    Separation best{-std::numeric_limits<float>::infinity(), vec3(1, 0, 0), b.pose.position};
    auto axis = [&](vec3 n) {
        float length = glm::length(n);
        if (length < Epsilon)
            return;
        n /= length;
        float radiusA = 0, radiusB = 0;
        for (int i = 0; i < 3; ++i) {
            radiusA += std::abs(glm::dot(n, axesA[i])) * a.shape.halfExtents[i];
            radiusB += std::abs(glm::dot(n, axesB[i])) * b.shape.halfExtents[i];
        }
        float signedCenter = glm::dot(a.pose.position - b.pose.position, n);
        float distance = std::abs(signedCenter) - radiusA - radiusB;
        if (distance > best.distance)
            best = {distance, signedCenter < 0 ? -n : n, b.pose.position};
    };
    for (int i = 0; i < 3; ++i) {
        axis(axesA[i]);
        axis(axesB[i]);
        for (int j = 0; j < 3; ++j)
            axis(glm::cross(axesA[i], axesB[j]));
    }
    best.point = supportPoint(b, best.normal);
    return best;
}
struct RotationSweep {
    quat target;
    vec3 axis{0, 1, 0};
    float angle = 0;
    RotationSweep(quat start, quat end) : target(glm::dot(start, end) < 0 ? -end : end) {
        auto relative = target * glm::conjugate(start);
        vec3 vector(relative.x, relative.y, relative.z);
        float sine = glm::length(vector);
        angle = 2 * std::atan2(sine, std::max(0.f, relative.w));
        if (sine > Epsilon)
            axis = vector / sine;
    }
};
bool rayBox(vec3 o, vec3 d, vec3 h, float limit, float& t, vec3& normal) {
    float lo = 0, hi = limit;
    normal = -d;
    for (int a = 0; a < 3; ++a) {
        if (std::abs(d[a]) < Epsilon) {
            if (std::abs(o[a]) > h[a])
                return false;
        } else {
            float near = (-h[a] - o[a]) / d[a], far = (h[a] - o[a]) / d[a];
            float sign = -1;
            if (near > far) {
                std::swap(near, far);
                sign = 1;
            }
            if (near > lo) {
                lo = near;
                normal = vec3(0);
                normal[a] = sign;
            }
            hi = std::min(hi, far);
            if (lo > hi)
                return false;
        }
    }
    t = lo;
    return lo <= limit;
}
bool rayCapsule(vec3 o, vec3 d, float r, float segment, float limit, float& t, vec3& normal) {
    vec3 closest(0, std::clamp(o.y, -segment, segment), 0);
    if (glm::dot(o - closest, o - closest) <= r * r) {
        t = 0;
        normal = -d;
        return true;
    }
    t = limit + 1;
    float a = d.x * d.x + d.z * d.z, b = o.x * d.x + o.z * d.z, c = o.x * o.x + o.z * o.z - r * r,
          disc = b * b - a * c;
    if (a > Epsilon && disc >= 0) {
        float hit = (-b - std::sqrt(disc)) / a, y = o.y + hit * d.y;
        if (hit >= 0 && std::abs(y) <= segment)
            t = hit;
    }
    for (float y : {-segment, segment}) {
        vec3 offset = o - vec3(0, y, 0);
        float proj = glm::dot(offset, d), discriminant = proj * proj - glm::dot(offset, offset) + r * r;
        if (discriminant < 0)
            continue;
        float hit = -proj - std::sqrt(discriminant);
        if (hit >= 0)
            t = std::min(t, hit);
    }
    if (t > limit)
        return false;
    vec3 point = o + t * d;
    normal = glm::normalize(point - vec3(0, std::clamp(point.y, -segment, segment), 0));
    return true;
}
} // namespace
ColliderShape ColliderShape::box(vec3 h) {
    ColliderShape s;
    s.halfExtents = h;
    validate(s);
    return s;
}
ColliderShape ColliderShape::capsule(float r, float height) {
    ColliderShape s;
    s.type = ColliderType::Capsule;
    s.radius = r;
    s.halfSegment = height * .5f - r;
    validate(s);
    return s;
}
void PhysicsScene::checkThread() const {
    if (std::this_thread::get_id() != owner_)
        throw std::logic_error("PhysicsScene used outside its owner thread");
}
bool PhysicsScene::contains(BodyHandle h) const {
    checkThread();
    return h.slot < slots_.size() && slots_[h.slot].alive && slots_[h.slot].generation == h.generation;
}
const PhysicsScene::Slot& PhysicsScene::require(BodyHandle h) const {
    if (!contains(h))
        throw std::out_of_range("Stale or invalid physics body handle");
    return slots_[h.slot];
}
PhysicsScene::Slot& PhysicsScene::require(BodyHandle h) {
    return const_cast<Slot&>(static_cast<const PhysicsScene&>(*this).require(h));
}
BodyHandle PhysicsScene::create(const PhysicsBody& b) {
    checkThread();
    validate(b.shape);
    validate(b.pose);
    uint32_t index;
    if (free_.empty()) {
        index = uint32_t(slots_.size());
        slots_.push_back({});
    } else {
        index = free_.back();
        free_.pop_back();
    }
    auto& s = slots_[index];
    s.body = b;
    s.bounds = bodyBounds(b);
    s.alive = true;
    s.leaf = b.enabled ? broadphase_.insert(index, s.bounds) : -1;
    ++revision_;
    return {index, s.generation};
}
void PhysicsScene::destroy(BodyHandle h) {
    auto& s = require(h);
    if (s.leaf != -1)
        broadphase_.remove(s.leaf);
    s.leaf = -1;
    s.alive = false;
    ++s.generation;
    if (!s.generation)
        ++s.generation;
    free_.push_back(h.slot);
    ++revision_;
}
const PhysicsBody& PhysicsScene::body(BodyHandle h) const {
    return require(h).body;
}
void PhysicsScene::setPose(BodyHandle h, const PhysicsPose& p) {
    validate(p);
    auto& s = require(h);
    if (s.body.pose.position == p.position && s.body.pose.rotation == p.rotation)
        return;
    s.body.pose = p;
    s.bounds = bodyBounds(s.body);
    if (s.leaf != -1)
        broadphase_.update(s.leaf, s.bounds);
    ++revision_;
}
void PhysicsScene::setShape(BodyHandle h, const ColliderShape& shape) {
    validate(shape);
    auto& s = require(h);
    s.body.shape = shape;
    s.bounds = bodyBounds(s.body);
    if (s.leaf != -1)
        broadphase_.update(s.leaf, s.bounds);
    ++revision_;
}
void PhysicsScene::setProperties(BodyHandle h, uint32_t layer, bool blocking, bool walkable, bool pickable) {
    auto& b = require(h).body;
    b.layer = layer;
    b.blocking = blocking;
    b.walkable = walkable;
    b.pickable = pickable;
    ++revision_;
}
void PhysicsScene::setMotion(BodyHandle h, BodyMotion motion) {
    auto& b = require(h).body;
    if (b.motion != motion) {
        b.motion = motion;
        ++revision_;
    }
}
void PhysicsScene::setEnabled(BodyHandle h, bool enabled) {
    auto& s = require(h);
    auto& b = s.body;
    if (b.enabled != enabled) {
        b.enabled = enabled;
        if (enabled)
            s.leaf = broadphase_.insert(h.slot, s.bounds);
        else {
            broadphase_.remove(s.leaf);
            s.leaf = -1;
        }
        ++revision_;
    }
}
uint64_t PhysicsScene::revision() const {
    checkThread();
    return revision_;
}
size_t PhysicsScene::size() const {
    checkThread();
    return slots_.size() - free_.size();
}
PhysicsBounds PhysicsScene::bounds(BodyHandle h) const {
    return require(h).bounds;
}
void PhysicsScene::rebuildBroadphase() {
    checkThread();
    broadphase_.rebuild();
}
BroadphaseStatistics PhysicsScene::broadphaseStatistics() const {
    checkThread();
    return broadphase_.statistics();
}
bool PhysicsScene::accepts(const PhysicsBody& b, QueryFilter f) {
    return b.enabled && (b.layer & f.mask) && (!f.ignoreOwner || b.owner != f.ignoreOwner) &&
           (!f.blockingOnly || b.blocking) && (!f.walkableOnly || b.walkable) &&
           (!f.pickableOnly || b.pickable);
}
std::vector<BodyHandle> PhysicsScene::bodies(QueryFilter f) const {
    checkThread();
    std::vector<BodyHandle> result;
    for (uint32_t i = 0; i < slots_.size(); ++i)
        if (slots_[i].alive && accepts(slots_[i].body, f))
            result.push_back({i, slots_[i].generation});
    return result;
}
std::optional<PhysicsHit> PhysicsScene::raycast(vec3 o, vec3 d, float limit, QueryFilter f) const {
    checkThread();
    if (!finite(o) || !finite(d) || !std::isfinite(limit) || limit < 0 || glm::length(d) < Epsilon)
        throw std::invalid_argument("Invalid physics ray");
    d = glm::normalize(d);
    std::optional<PhysicsHit> result;
    for (uint32_t i : broadphase_.raycast(o, d, limit)) {
        const auto& s = slots_[i];
        if (!s.alive || !accepts(s.body, f))
            continue;
        float broadT;
        vec3 broadN;
        if (!rayBox(o - (s.bounds.min + s.bounds.max) * .5f, d, (s.bounds.max - s.bounds.min) * .5f, limit,
                    broadT, broadN))
            continue;
        const auto& b = s.body;
        quat inverse = glm::conjugate(b.pose.rotation);
        vec3 localO = inverse * (o - b.pose.position), localD = inverse * d, normal;
        float t;
        bool hit = b.shape.type == ColliderType::Box
                       ? rayBox(localO, localD, b.shape.halfExtents, limit, t, normal)
                       : rayCapsule(localO, localD, b.shape.radius, b.shape.halfSegment, limit, t, normal);
        if (hit && (!result || t < limit)) {
            limit = t;
            result = PhysicsHit{{i, s.generation}, b.owner, o + d * t, b.pose.rotation * normal, t, 0};
        }
    }
    return result;
}
std::vector<PhysicsHit> PhysicsScene::overlapCapsule(const CapsuleQuery& c, QueryFilter f) const {
    checkThread();
    validate(c);
    std::vector<PhysicsHit> result;
    auto broad = capsuleBounds(c);
    for (uint32_t i : broadphase_.overlap(broad)) {
        const auto& s = slots_[i];
        if (!s.alive || !accepts(s.body, f) || !intersects(broad, s.bounds))
            continue;
        auto contact = separation(c, s.body);
        if (contact.distance < -Epsilon)
            result.push_back(
                {{i, s.generation}, s.body.owner, contact.point, contact.normal, contact.distance, 0});
    }
    return result;
}
std::optional<PhysicsHit> PhysicsScene::sweepCapsule(const CapsuleQuery& c, vec3 delta, QueryFilter f) const {
    checkThread();
    validate(c);
    if (!finite(delta))
        throw std::invalid_argument("Invalid sweep delta");
    float length = glm::length(delta);
    if (length < Epsilon)
        return {};
    auto broad = capsuleBounds(c, delta);
    std::optional<PhysicsHit> result;
    float closest = 1;
    for (uint32_t i : broadphase_.overlap(broad)) {
        const auto& s = slots_[i];
        if (!s.alive || !accepts(s.body, f) || !intersects(broad, s.bounds))
            continue;
        float t = 0;
        for (int iteration = 0; iteration < 64 && t <= closest; ++iteration) {
            CapsuleQuery probe = c;
            probe.center += delta * t;
            auto contact = separation(probe, s.body);
            float closing = -glm::dot(delta, contact.normal);
            // Convex shapes: a supporting plane separating a tangent/outward ray cannot be crossed later.
            if (closing <= Epsilon && contact.distance >= -Epsilon)
                break;
            if (contact.distance <= Epsilon || iteration == 63) {
                closest = t;
                result =
                    PhysicsHit{{i, s.generation}, s.body.owner, contact.point, contact.normal, length * t, t};
                break;
            }
            if (closing <= Epsilon)
                break;
            t += std::max(contact.distance / closing, 1e-7f);
        }
    }
    return result;
}
MoveResult PhysicsScene::moveAndSlide(const CapsuleQuery& c, vec3 delta, QueryFilter f) const {
    checkThread();
    validate(c);
    if (!finite(delta))
        throw std::invalid_argument("Invalid movement");
    MoveResult result;
    result.position = c.center;
    vec3 remaining = delta;
    for (int i = 0; i < 5 && glm::length(remaining) > Epsilon; ++i) {
        CapsuleQuery probe = c;
        probe.center = result.position;
        auto hit = sweepCapsule(probe, remaining, f);
        if (!hit) {
            result.position += remaining;
            break;
        }
        result.blocked = true;
        result.contacts.push_back(*hit);
        float travel = std::max(0.f, hit->fraction - Skin / glm::length(remaining));
        result.position += remaining * travel;
        remaining *= 1 - travel;
        remaining -= hit->normal * std::min(0.f, glm::dot(remaining, hit->normal));
        if (travel == 0 && hit->distance < -Epsilon)
            break;
    }
    result.applied = result.position - c.center;
    return result;
}
std::vector<PhysicsHit> PhysicsScene::overlapShape(const ShapeQuery& query, QueryFilter filter) const {
    checkThread();
    validate(query.shape);
    validate(query.pose);
    PhysicsBody body;
    body.shape = query.shape;
    body.pose = query.pose;
    auto broad = bodyBounds(body);
    std::vector<PhysicsHit> hits;
    for (auto i : broadphase_.overlap(broad)) {
        const auto& s = slots_[i];
        if (!s.alive || !accepts(s.body, filter) || !intersects(broad, s.bounds))
            continue;
        auto contact = separation(query, s.body);
        if (contact.distance < -Epsilon)
            hits.push_back({{i, s.generation}, s.body.owner, contact.point, contact.normal,
                            contact.distance, 0});
    }
    return hits;
}
std::optional<PhysicsHit> PhysicsScene::sweepShape(const ShapeQuery& query, vec3 delta, quat target,
                                                  QueryFilter filter) const {
    checkThread();
    validate(query.shape);
    validate(query.pose);
    validate(PhysicsPose{query.pose.position + delta, target});
    RotationSweep rotation(query.pose.rotation, target);
    float length = glm::length(delta), radius = shapeRadius(query.shape);
    if (length + rotation.angle * radius < Epsilon)
        return {};
    // The bounding sphere encloses intermediate orientations, including a half-turn
    // whose start and end boxes occupy exactly the same space.
    vec3 extent(radius + Skin);
    PhysicsBounds broad{glm::min(query.pose.position, query.pose.position + delta) - extent,
                         glm::max(query.pose.position, query.pose.position + delta) + extent};
    std::optional<PhysicsHit> hit;
    float closest = 1;
    for (auto i : broadphase_.overlap(broad)) {
        const auto& s = slots_[i];
        if (!s.alive || !accepts(s.body, filter) || !intersects(broad, s.bounds))
            continue;
        float t = 0;
        for (int iteration = 0; iteration < 128 && t <= closest; ++iteration) {
            ShapeQuery probe = query;
            probe.pose.position += delta * t;
            probe.pose.rotation = glm::normalize(glm::slerp(query.pose.rotation, rotation.target, t));
            auto contact = separation(probe, s.body);
            // A bound on support motion along this fixed separating plane, valid for
            // the entire remaining rotation. Rotation about a floor normal is tangent.
            float closing = -glm::dot(delta, contact.normal) +
                            rotation.angle * radius * glm::length(glm::cross(rotation.axis, contact.normal));
            if (closing <= Epsilon && contact.distance >= -Epsilon)
                break;
            if (contact.distance <= Epsilon || iteration == 127) {
                closest = t;
                hit = PhysicsHit{{i, s.generation}, s.body.owner, contact.point, contact.normal,
                                  length * t, t};
                break;
            }
            if (closing <= Epsilon)
                break;
            t += contact.distance / closing;
        }
    }
    return hit;
}
BodyMoveResult PhysicsScene::moveAndSlide(const ShapeQuery& query, vec3 delta, quat target,
                                          QueryFilter filter) const {
    // sweepShape validates the complete request, including zero-length movements.
    auto hit = sweepShape(query, delta, target, filter);
    RotationSweep rotation(query.pose.rotation, target);
    BodyMoveResult result;
    result.pose = query.pose;
    vec3 remaining = delta;
    for (int i = 0; i < 5; ++i) {
        if (!hit) {
            result.pose.position += remaining;
            if (i == 0)
                result.pose.rotation = rotation.target;
            break;
        }
        result.blocked = true;
        result.contacts.push_back(*hit);
        float motion = glm::length(remaining) + (i == 0 ? rotation.angle * shapeRadius(query.shape) : 0);
        float travel = std::max(0.f, hit->fraction - Skin / std::max(motion, Epsilon));
        result.pose.position += remaining * travel;
        if (i == 0)
            result.pose.rotation = glm::normalize(glm::slerp(query.pose.rotation, rotation.target, travel));
        // At contact preserve accepted orientation; slide only the remaining translation.
        remaining *= 1 - travel;
        remaining -= hit->normal * std::min(0.f, glm::dot(remaining, hit->normal));
        if (glm::length(remaining) < Epsilon)
            break;
        hit = sweepShape({query.shape, result.pose}, remaining, result.pose.rotation, filter);
    }
    result.applied = result.pose.position - query.pose.position;
    return result;
}
bool PhysicsScene::resizeCharacter(BodyHandle h, float height) {
    const auto previous = body(h);
    if (previous.shape.type != ColliderType::Capsule)
        throw std::invalid_argument("Character must be a capsule");
    if (glm::distance(previous.pose.rotation * vec3(0, 1, 0), vec3(0, 1, 0)) > .001f)
        throw std::invalid_argument("Character controller requires an upright capsule");
    auto shape = ColliderShape::capsule(previous.shape.radius, height);
    if (std::abs(height - previous.shape.height()) < Epsilon)
        return true;
    PhysicsPose pose = previous.pose;
    pose.position.y += (height - previous.shape.height()) * .5f;
    QueryFilter filter;
    filter.blockingOnly = true;
    filter.ignoreOwner = previous.owner;
    auto overlaps = overlapCapsule({pose.position, shape.radius, height}, filter);
    if (std::any_of(overlaps.begin(), overlaps.end(),
                    [&](const PhysicsHit& hit) { return !(hit.body == h); }))
        return false;
    setShape(h, shape);
    setPose(h, pose);
    return true;
}
} // namespace afterlight
