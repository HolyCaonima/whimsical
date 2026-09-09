#pragma once
#include "Math.h"
#include <cmath>
#include <stdexcept>
#include <string>

namespace whimsical {
// The packed GPU ABI and sampling math are shared with GLSL, avoiding a second
// reference implementation that can agree with tests while the renderer diverges.
#include "lighting/LightSampling.h"
static_assert(sizeof(Light) == 96, "Light must match std430");

enum class LightType { Directional, Spot, Point, Rect, Capsule };
struct LightComponent {
    vec3 color{1}; // Nonnegative linear RGB energy proportions, normalized on extraction.
    float intensity = 1, radius = .1f;
    LightType type = LightType::Point;
    float width = 1, height = 1, length = 1;
    float innerAngle = .35f, outerAngle = .6f, angularRadius = .00465f; // radians, half angles
    bool twoSided = false;
};
inline const char* lightTypeName(LightType type) {
    switch (type) {
    case LightType::Directional: return "directional";
    case LightType::Spot: return "spot";
    case LightType::Point: return "point";
    case LightType::Rect: return "rect";
    case LightType::Capsule: return "capsule";
    }
    throw std::invalid_argument("Unknown light type");
}
inline LightType parseLightType(const std::string& name) {
    for (auto type : {LightType::Directional, LightType::Spot, LightType::Point, LightType::Rect, LightType::Capsule})
        if (name == lightTypeName(type)) return type;
    throw std::invalid_argument("Unknown light type: " + name);
}
inline void validateLight(const LightComponent& v) {
    lightTypeName(v.type);
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(v.color[i]) || v.color[i] < 0) throw std::invalid_argument("Invalid light color");
    for (float x : {v.intensity, v.radius, v.width, v.height, v.length, v.innerAngle, v.outerAngle, v.angularRadius})
        if (!std::isfinite(x) || x < 0) throw std::invalid_argument("Light parameters must be finite and nonnegative");
    if (v.type == LightType::Rect && (v.width == 0 || v.height == 0))
        throw std::invalid_argument("Rect dimensions must be positive");
    if (v.type == LightType::Capsule && v.radius == 0)
        throw std::invalid_argument("Capsule radius must be positive");
    if (v.type == LightType::Spot && (v.outerAngle <= 0 || v.outerAngle > Pi*.5f || v.innerAngle > v.outerAngle))
        throw std::invalid_argument("Spot requires 0 <= innerAngle <= outerAngle <= pi/2, outerAngle > 0");
    if (v.angularRadius >= Pi*.5f) throw std::invalid_argument("Directional angular radius must be < pi/2");
}
inline Light packLight(const LightComponent& v, vec3 position, quat rotation) {
    Light l{};
    l.positionRadius = vec4(position, v.radius);
    float colorSum = v.color.x+v.color.y+v.color.z;
    l.colorIntensity = vec4(colorSum > 0 ? v.color/colorSum : vec3(0), v.intensity);
    l.directionType = vec4(rotation*vec3(0,0,1), float(v.type));
    l.tangentWidth = vec4(rotation*vec3(1,0,0), v.width);
    float si = std::sin(v.innerAngle*.5f), so = std::sin(v.outerAngle*.5f);
    float qi = 2*si*si, qo = 2*so*so, d = qo-qi;
    l.shape = vec4(v.type == LightType::Capsule ? v.length : v.height, qi, qo, v.twoSided ? 1.f : 0.f);
    float area = 0, normalization = 4*Pi;
    if (v.type == LightType::Point && v.radius > 0) { area = 4*Pi*v.radius*v.radius; normalization = Pi; }
    if (v.type == LightType::Rect) { area = v.width*v.height; normalization = v.twoSided ? 2*Pi : Pi; }
    if (v.type == LightType::Capsule) { area = 2*Pi*v.radius*(v.length+2*v.radius); normalization = Pi; }
    if (v.type == LightType::Spot) {
        // Integrals of smoothstep in cos(theta): dOmega for a point, cos(theta)dOmega for a disk.
        area = Pi*v.radius*v.radius;
        normalization = v.radius == 0 ? Pi*(qi+qo) : 2*Pi*(qi*(2-qi)*.5f+d*((1-qo)*.5f+.35f*d));
    }
    float sinHalf = std::sin(v.angularRadius*.5f);
    l.emission = vec4(area, normalization, 2*sinHalf*sinHalf, 0);
    return l;
}
// A proposal heuristic only. Directional power is measured through a fixed 1 m^2
// aperture; the uniform mixture keeps all emitters reachable regardless of scale.
inline float lightProposalPower(const Light& l) {
    return glm::dot(vec3(l.colorIntensity), vec3(.2126f,.7152f,.0722f))*l.colorIntensity.w;
}
} // namespace whimsical
