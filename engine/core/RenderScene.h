#pragma once
#include "Types.h"
#include <vector>
#include <cstdint>
namespace whimsical {
// The persistent, slot-addressed description of everything the renderer can draw.
//
// A slot is stable for the whole lifetime of a proxy. The renderer maps each proxy to
// a range of GPU and TLAS instances. Raster draws address those through a transient
// instance stream, so sorting and batching never change an object's identity.
// Spawning, hiding or moving an object touches only that object's persistent slot.
//
// Changes are recorded as they happen rather than rediscovered by comparing whole frames,
// and they are classified, because a transform changes orders of magnitude more often
// than a mesh or material binding. `moved` drives a fast path that rewrites only the
// transform half of an instance and leaves geometry, material and flags untouched.
//
// The delta lists say *which* slots changed, never what they changed to; values are read
// from the proxy array in the published snapshot. Mutation order within a tick is
// therefore irrelevant, duplicate marks are harmless, and a consumer that missed a
// snapshot can recover by rereading the array.
class RenderScene {
    enum Mark : uint8_t { MarkStructural = 1, MarkMoved = 2, MarkAttributes = 4 };
    std::vector<RenderProxy> proxies_;
    std::vector<uint8_t> marks_;
    std::vector<uint32_t> free_;
    struct MaterialSlot {
        std::shared_ptr<const MaterialAsset> asset;
        uint32_t users = 0;
    };
    std::vector<MaterialSlot> materialSlots_;
    std::vector<Material> materials_;
    uint32_t retainMaterial(const std::shared_ptr<const MaterialAsset>&);
    void releaseMaterial(uint32_t);
    SceneDelta pending_;
    uint64_t revision_ = 0, published_ = 0, topology_ = 0;
    void mark(uint32_t slot, Mark kind);

  public:
    void exchangeScene(RenderScene&);
    const std::vector<RenderProxy>& proxies() const {
        return proxies_;
    }
    const std::vector<Material>& materials() const {
        return materials_;
    }
    uint32_t capacity() const {
        return uint32_t(proxies_.size());
    }
    uint64_t revision() const {
        return revision_;
    }
    const RenderProxy& proxy(uint32_t slot) const;
    uint32_t create(const ProxyTransform&, ProxyAttributes, const std::shared_ptr<const MaterialAsset>&);
    void destroy(uint32_t slot);
    // Fast path. Does nothing when the transform is unchanged, so a scene that is being
    // simulated without actually moving produces an empty delta and no GPU traffic.
    void setTransform(uint32_t slot, const ProxyTransform&);
    static void validateInstances(uint32_t count, const std::vector<ProxyTransform>&);
    void setInstances(uint32_t slot, uint32_t count, const std::vector<ProxyTransform>&);
    void setAttributes(uint32_t slot, ProxyAttributes, const std::shared_ptr<const MaterialAsset>&);
    void setVisible(uint32_t slot, bool visible);
    void geometryChanged(uint32_t slot);
    // Hands the accumulated events to a snapshot and opens a new accumulation window.
    SceneDelta publish();
};
} // namespace whimsical
