#pragma once
#include "ConsoleTypes.h"
#include "RenderTarget.h"
#include "Input.h"
#include "Math.h"
#include "Light.h"
#include "assets/Material.h"
#include "animation/Animation.h"
#include <vector>
#include <array>
#include <memory>
#include <string>
#include <cstdint>
#include <optional>
#include <algorithm>
namespace afterlight {
namespace ui {
struct UiFrame;
}
struct SkinnedMesh;
struct StaticMesh;
struct MaterialAsset;
struct TextureAsset;
struct CpuProfile;

struct RenderComponent {
    std::shared_ptr<const MaterialAsset> material;
    bool visible = true;
    bool castShadow = true;
    bool overlay = false; // Unlit, vertex-coloured helper geometry, outside scene lighting.
    vec3 overlayColor{1};
};
// A renderable occupies a stable slot for its whole lifetime. Transform and attributes
// are separate because a transform changes orders of magnitude more often, and the two
// map onto disjoint halves of a GPU instance so each can be rewritten on its own.
struct ProxyTransform {
    vec3 position{0};
    vec3 scale{1};
    quat rotation{1, 0, 0, 0};
    bool operator==(const ProxyTransform& o) const {
        return position == o.position && scale == o.scale && rotation == o.rotation;
    }
    bool operator!=(const ProxyTransform& o) const {
        return !(*this == o);
    }
};
struct ProxyAttributes {
    uint32_t entity = 0;
    uint32_t material = 0;
    bool visible = true;
    bool castShadow = true;
    bool overlay = false;
    vec3 overlayColor{1};
    bool operator==(const ProxyAttributes& o) const {
        return entity == o.entity && material == o.material && visible == o.visible &&
               castShadow == o.castShadow && overlay == o.overlay &&
               overlayColor == o.overlayColor;
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
    std::vector<uint32_t> attributes; // mesh, material, visibility
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
// Presentation coordinates are window pixels. Zero size selects the whole window.
struct ViewRect {
    uint32_t x = 0, y = 0, width = 0, height = 0;
    ViewRect fit(uint32_t surfaceWidth, uint32_t surfaceHeight) const {
        if (!surfaceWidth || !surfaceHeight)
            return {};
        if (!width && !height && !x && !y)
            return {0, 0, surfaceWidth, surfaceHeight};
        auto left = std::min(x, surfaceWidth), top = std::min(y, surfaceHeight);
        return {left, top, std::min(width, surfaceWidth - left), std::min(height, surfaceHeight - top)};
    }
    bool contains(float px, float py) const {
        return px >= x && py >= y && px < double(x) + width && py < double(y) + height;
    }
};
// A host-owned observation of a scene. Neither the rectangle nor an override camera is
// authored scene data. The initial single-view host follows the map camera by default.
struct EntityOutline {
    uint32_t entity = 0;
    vec4 color{1}; // Display-space RGBA; application chooses all visual policy.
};
struct RenderView {
    ViewRect rectangle;
    std::optional<Camera> camera;
    std::vector<EntityOutline> outlines;
};
struct AnimationInspection {
    uint32_t entity = 0;
    std::string name, solver;
    std::vector<animation::EnumAttribute> schema;
    animation::AttributeValues values;
};
struct Frame {
    std::vector<std::shared_ptr<RenderTargetResource>> entityIDOutputs;
    std::vector<std::shared_ptr<PixelReadRequest>> pixelReads;
    std::shared_ptr<const ui::UiFrame> ui;
    uint64_t gpuProfileRequest = 0;
    std::shared_ptr<const CpuProfile> cpuProfile;
    ConsoleView console;
    bool hudEnabled = true, statsEnabled = false, forceFullUpload = false;
    float exposure = 1.15f;
    bool diHistoryConfidence = true;
    struct StaticDraw {
        uint32_t slot = 0;
        std::shared_ptr<const StaticMesh> mesh;
    };
    std::vector<StaticDraw> staticMeshes;
    struct Skin {
        uint32_t slot = 0;
        std::shared_ptr<const SkinnedMesh> mesh;
        std::vector<mat4> palette;
    };
    std::vector<Skin> skins;
    struct SkeletonPose {
        uint32_t owner = 0;
        std::vector<mat4> jointWorld;
    };
    std::vector<SkeletonPose> skeletons; // Immutable animation output for render consumers.
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
    std::vector<Material> materials;
    std::vector<Light> lights;
    std::vector<uint32_t> lightEntities; // Packed light identity for temporal history.
    Camera camera;
    ViewRect viewport;
    Input input;
    std::vector<EntityOutline> outlines; // Sorted by entity, unique, visible scene geometry only.
    uint64_t tick = 0;
    double time = 0;
    bool resetHistory = false;
    int debugView = 0;
};
// Snapshots are immutable once published and shared by reference count, so neither
// thread deep-copies a Frame and the renderer can hold on to older ones for free.
using FrameRef = std::shared_ptr<const Frame>;
inline mat4 transform(const ProxyTransform& t) {
    return glm::translate(mat4(1), t.position) * glm::mat4_cast(t.rotation) * glm::scale(mat4(1), t.scale);
}
} // namespace afterlight
