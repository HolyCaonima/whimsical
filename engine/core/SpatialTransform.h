#pragma once
#include "Math.h"
#include <stdexcept>

namespace whimsical {
// Local and world transforms are TRS. Hierarchies never introduce shear.
struct TransformPose {
    vec3 position{0};
    quat rotation{1, 0, 0, 0};
    vec3 scale{1};
};
inline mat4 transformMatrix(const TransformPose& p) {
    return glm::translate(mat4(1), p.position) * glm::mat4_cast(p.rotation) * glm::scale(mat4(1), p.scale);
}
inline void validateTransformPose(const TransformPose& p) {
    float q = glm::dot(p.rotation, p.rotation);
    if (!std::isfinite(q) || std::abs(q - 1) > .001f)
        throw std::invalid_argument("Transform requires a unit rotation");
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(p.position[i]) || !std::isfinite(p.scale[i]) || p.scale[i] <= 0)
            throw std::invalid_argument("Transform requires finite position and positive scale");
}
// FTransform-style composition: discard shear by composing TRS directly.
inline TransformPose composeTransform(const TransformPose& parent, const TransformPose& local) {
    return {parent.position + parent.rotation * (parent.scale * local.position),
            glm::normalize(parent.rotation * local.rotation), parent.scale * local.scale};
}
inline TransformPose relativeTransform(const TransformPose& parent, const TransformPose& world) {
    auto inverse = glm::inverse(parent.rotation);
    return {(inverse * (world.position - parent.position)) / parent.scale,
            glm::normalize(inverse * world.rotation), world.scale / parent.scale};
}
} // namespace whimsical
