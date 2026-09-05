#pragma once
#include "Math.h"
#include <vector>
#include <array>
#include <memory>
#include <string>
#include <cstdint>
namespace afterlight {
struct Material {
    vec4 albedoRoughness{.5f, .5f, .5f, .5f};
    vec4 emissionMetallic{0};
};
enum class Shape : uint32_t { Box, Capsule };
// Render-only value snapshot. Physics owns separate shapes, poses, filters and lifetimes.
struct RenderObject {
    uint32_t id = 0;
    Shape shape = Shape::Box;
    vec3 position{0}, scale{1};
    float yaw = 0;
    uint32_t material = 0;
    bool interactable = false, enabled = true;
    std::string name;
};
struct alignas(16) Light {
    vec4 positionRadius{0, 5, 0, .4f};
    vec4 colorIntensity{1, 1, 1, 40};
};
struct Camera {
    vec3 target{0, 0, 0};
    float yaw = .72f, pitch = .86f, distance = 24, fov = .62f;
    mat4 view() const {
        vec3 offset{std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
        return glm::lookAt(target + offset * distance, target, vec3(0, 1, 0));
    }
    vec3 eye() const {
        return vec3(glm::inverse(view())[3]);
    }
    mat4 projection(float aspect) const {
        auto p = glm::perspective(fov, aspect, .1f, 160.f);
        p[1][1] *= -1;
        return p;
    }
};
struct Input {
    std::array<bool, 256> keys{}, pressed{};
    bool left = false, right = false, middle = false, leftPressed = false, rightPressed = false,
         focused = true;
    float mouseX = 0, mouseY = 0, deltaX = 0, deltaY = 0, wheel = 0;
    uint32_t width = 1280, height = 800;
};
struct Frame {
    struct DebugLine {
        vec3 a, b, color;
    };
    std::vector<DebugLine> physicsLines;
    bool physicsDebug = false;
    std::vector<RenderObject> entities;
    struct MapObstacle {
        vec3 position, size;
    };
    std::vector<MapObstacle> mapObstacles;
    std::vector<Material> materials;
    std::vector<Light> lights;
    Camera camera;
    Input input;
    vec3 player{0}, destination{0};
    std::vector<vec3> path;
    uint32_t selected = 0, hovered = 0;
    std::string locomotion = "Idle", message = "Select your companion";
    uint64_t tick = 0;
    double time = 0;
    bool resetHistory = false, hasDestination = false;
    int debugView = 0;
};
// Snapshots are immutable once published and shared by reference count, so neither
// thread deep-copies a Frame and the renderer can hold on to older ones for free.
using FrameRef = std::shared_ptr<const Frame>;
inline mat4 transform(const RenderObject& e) {
    return glm::translate(mat4(1), e.position) * glm::rotate(mat4(1), e.yaw, vec3(0, 1, 0)) *
           glm::scale(mat4(1), e.scale);
}
} // namespace afterlight
