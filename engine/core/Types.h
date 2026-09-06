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
// A renderable occupies a stable slot for its whole lifetime. Transform and attributes
// are separate because a transform changes orders of magnitude more often, and the two
// map onto disjoint halves of a GPU instance so each can be rewritten on its own.
struct ProxyTransform {
    vec3 position{0};
    vec3 scale{1};
    float yaw = 0;
    bool operator==(const ProxyTransform& o) const {
        return position == o.position && scale == o.scale && yaw == o.yaw;
    }
    bool operator!=(const ProxyTransform& o) const {
        return !(*this == o);
    }
};
struct ProxyAttributes {
    uint32_t entity = 0;
    uint32_t material = 0;
    Shape shape = Shape::Box;
    bool visible = true;
    bool interactable = false;
    bool operator==(const ProxyAttributes& o) const {
        return entity == o.entity && material == o.material && shape == o.shape &&
               visible == o.visible && interactable == o.interactable;
    }
    bool operator!=(const ProxyAttributes& o) const {
        return !(*this == o);
    }
};
// Deliberately free of heap-owning members: publishing a snapshot is then one flat copy,
// not one allocation per object.
struct RenderProxy {
    ProxyTransform transform;
    ProxyAttributes attributes;
    bool live = false;
};
// Slots touched since the previous published delta. `base` is the revision that delta
// ended at, so a consumer whose mirror sits exactly at `base` may apply these lists and
// otherwise must resynchronise from the proxy array.
struct SceneDelta {
    uint64_t base = 0, revision = 0;
    // Appeared, disappeared, or reused: rewrite the instance whole, geometry included.
    std::vector<uint32_t> structural;
    std::vector<uint32_t> moved;      // transform only, the fast path
    std::vector<uint32_t> attributes; // mesh, material, visibility, interactable
    // Bumped when the slot count grows or a slot rebinds to different geometry, the one
    // kind of change an incremental acceleration structure update cannot absorb. It is a
    // running count rather than a per-delta flag so that it survives snapshots a consumer
    // skipped: a flag raised in a snapshot nobody read would be lost, and the consumer
    // would keep refitting a structure whose topology had moved out from under it.
    uint64_t topology = 0;
    bool empty() const {
        return structural.empty() && moved.empty() && attributes.empty();
    }
    // Absorbs a delta that was published just before this one and never delivered, so the
    // events it carried reach the consumer instead of dying with the snapshot. Without it
    // a dropped snapshot leaves the consumer's mirror stranded at a revision no delta
    // chains to, and its only recovery is to rewrite the entire scene.
    void prepend(const SceneDelta& dropped);
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
    // The persistent scene, indexed by stable slot, plus the events that changed it since
    // the previous snapshot. Values always come from `proxies`; `delta` only says which
    // slots to look at, so a consumer that skipped snapshots can fall back to the array.
    std::vector<RenderProxy> proxies;
    SceneDelta delta;
    // Resolved here rather than stored per proxy: keeping proxies free of heap-owning
    // members is what makes publishing a snapshot a single flat copy.
    std::string hoveredName;
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
inline mat4 transform(const ProxyTransform& t) {
    return glm::translate(mat4(1), t.position) * glm::rotate(mat4(1), t.yaw, vec3(0, 1, 0)) *
           glm::scale(mat4(1), t.scale);
}
} // namespace afterlight
