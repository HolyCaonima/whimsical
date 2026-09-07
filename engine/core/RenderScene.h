#pragma once
#include "Types.h"
#include <vector>
#include <cstdint>
namespace afterlight {
// The persistent, slot-addressed description of everything the renderer can draw.
//
// A slot is stable for the whole lifetime of a proxy. The renderer mirrors slots one to
// one onto GPU instance entries, TLAS instances and draw instance indices, so nothing
// downstream is rebuilt when the scene changes: spawning, hiding or moving an object
// touches only that object's slot.
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
    SceneDelta pending_;
    uint64_t revision_ = 0, published_ = 0, topology_ = 0;
    void mark(uint32_t slot, Mark kind);

  public:
    void exchangeScene(RenderScene&);
    const std::vector<RenderProxy>& proxies() const {
        return proxies_;
    }
    uint32_t capacity() const {
        return uint32_t(proxies_.size());
    }
    uint64_t revision() const {
        return revision_;
    }
    const RenderProxy& proxy(uint32_t slot) const;
    uint32_t create(const ProxyTransform&, const ProxyAttributes&);
    void destroy(uint32_t slot);
    // Fast path. Does nothing when the transform is unchanged, so a scene that is being
    // simulated without actually moving produces an empty delta and no GPU traffic.
    void setTransform(uint32_t slot, const ProxyTransform&);
    void setAttributes(uint32_t slot, const ProxyAttributes&);
    void setMaterial(uint32_t slot, uint32_t material);
    void setVisible(uint32_t slot, bool visible);
    void geometryChanged(uint32_t slot);
    // Hands the accumulated events to a snapshot and opens a new accumulation window.
    SceneDelta publish();
};
} // namespace afterlight
