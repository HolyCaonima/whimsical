#pragma once
#include "PhysicsScene.h"
#include "core/SpatialTransform.h"

namespace afterlight {
// Primitive collision supports orthogonal boxes and uniformly scaled capsules.
// Keep authoring dimensions separate from the derived world-space shape.
inline ColliderShape scaledCollider(const ColliderShape& local, vec3 scale) {
    if (local.type == ColliderType::Box)
        return ColliderShape::box(local.halfExtents * scale);
    if (std::abs(scale.x - scale.y) > .0001f * scale.x ||
        std::abs(scale.x - scale.z) > .0001f * scale.x)
        throw std::invalid_argument("Capsule collider requires uniform world scale");
    return ColliderShape::capsule(local.radius * scale.x, local.height() * scale.x);
}
inline ShapeQuery worldCollider(const ColliderShape& local, const TransformPose& p) {
    return {scaledCollider(local, p.scale), {p.position, p.rotation}};
}
} // namespace afterlight
