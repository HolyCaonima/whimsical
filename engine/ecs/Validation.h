#pragma once
#include "Components.h"
#include <stdexcept>

namespace afterlight {
inline void validateRigidPose(const PhysicsPose& p) {
    float q = glm::dot(p.rotation, p.rotation);
    if (!std::isfinite(p.position.x) || !std::isfinite(p.position.y) || !std::isfinite(p.position.z) ||
        !std::isfinite(q) || std::abs(q - 1) > .001f)
        throw std::invalid_argument("Expected finite position and unit rotation");
}
inline void validateRenderAppearance(const RenderComponent& r) {
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(r.scale[i]) || r.scale[i] <= 0 || !std::isfinite(r.animationScale[i]) ||
            r.animationScale[i] <= 0 || !std::isfinite(r.offset[i]))
            throw std::invalid_argument("Invalid render dimensions");
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(r.overlayColor[i]) || r.overlayColor[i] < 0 || r.overlayColor[i] > 1)
            throw std::invalid_argument("Overlay colour must be in [0, 1]");
}
} // namespace afterlight
