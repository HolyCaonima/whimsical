#pragma once
#include "Components.h"
#include <stdexcept>

namespace whimsical {
inline void validateRigidPose(const PhysicsPose& p) {
    float q = glm::dot(p.rotation, p.rotation);
    if (!std::isfinite(p.position.x) || !std::isfinite(p.position.y) || !std::isfinite(p.position.z) ||
        !std::isfinite(q) || std::abs(q - 1) > .001f)
        throw std::invalid_argument("Expected finite position and unit rotation");
}
} // namespace whimsical
