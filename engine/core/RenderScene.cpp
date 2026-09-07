#include "RenderScene.h"
#include <algorithm>
#include <stdexcept>
namespace afterlight {
void SceneDelta::prepend(const SceneDelta& dropped) {
    // Slots are only ever appended to the scene, so every index the dropped delta names is
    // still addressable in the newer snapshot's proxy array and still means the same slot.
    auto absorb = [](std::vector<uint32_t>& into, const std::vector<uint32_t>& from) {
        into.insert(into.end(), from.begin(), from.end());
        std::sort(into.begin(), into.end());
        into.erase(std::unique(into.begin(), into.end()), into.end());
    };
    absorb(structural, dropped.structural);
    absorb(moved, dropped.moved);
    absorb(attributes, dropped.attributes);
    // The merged delta now starts where the dropped one did, which is where a consumer
    // that never saw it still sits. `revision` and `topology` already describe the newer
    // state and are what the consumer should end up mirroring.
    base = dropped.base;
}
const RenderProxy& RenderScene::proxy(uint32_t slot) const {
    if (slot >= proxies_.size())
        throw std::out_of_range("Invalid render proxy slot");
    return proxies_[slot];
}
void RenderScene::mark(uint32_t slot, Mark kind) {
    // A slot already queued as structural is rewritten whole, so a finer grained mark on
    // top of it would only make the consumer do the same work twice.
    if ((marks_[slot] & MarkStructural) || (marks_[slot] & kind))
        return;
    marks_[slot] |= kind;
    if (kind == MarkStructural)
        pending_.structural.push_back(slot);
    else if (kind == MarkMoved)
        pending_.moved.push_back(slot);
    else
        pending_.attributes.push_back(slot);
}
uint32_t RenderScene::create(const ProxyTransform& transform, const ProxyAttributes& attributes) {
    uint32_t slot;
    if (free_.empty()) {
        slot = uint32_t(proxies_.size());
        proxies_.emplace_back();
        marks_.push_back(0);
        ++topology_; // The slot count grew.
    } else {
        slot = free_.back();
        free_.pop_back();
        // A reused slot can reference different geometry than it did before.
        if (proxies_[slot].attributes.shape != attributes.shape)
            ++topology_;
    }
    proxies_[slot] = {transform, attributes, true};
    ++revision_;
    mark(slot, MarkStructural);
    return slot;
}
void RenderScene::destroy(uint32_t slot) {
    if (!proxy(slot).live)
        return;
    auto& p = proxies_[slot];
    p.live = false;
    p.attributes.visible = false;
    free_.push_back(slot);
    ++revision_;
    mark(slot, MarkStructural);
}
void RenderScene::setTransform(uint32_t slot, const ProxyTransform& transform) {
    if (!proxy(slot).live || proxies_[slot].transform == transform)
        return;
    proxies_[slot].transform = transform;
    ++revision_;
    mark(slot, MarkMoved);
}
void RenderScene::setAttributes(uint32_t slot, const ProxyAttributes& attributes) {
    if (!proxy(slot).live || proxies_[slot].attributes == attributes)
        return;
    // Geometry is the one attribute a consumer cannot fold into an incremental update.
    if (proxies_[slot].attributes.shape != attributes.shape)
        ++topology_;
    proxies_[slot].attributes = attributes;
    ++revision_;
    mark(slot, MarkAttributes);
}
void RenderScene::setMaterial(uint32_t slot, uint32_t material) {
    auto attributes = proxy(slot).attributes;
    attributes.material = material;
    setAttributes(slot, attributes);
}
void RenderScene::setVisible(uint32_t slot, bool visible) {
    auto attributes = proxy(slot).attributes;
    attributes.visible = visible;
    setAttributes(slot, attributes);
}
void RenderScene::geometryChanged(uint32_t slot) {
    if (!proxy(slot).live) return;
    ++revision_;
    ++topology_;
    mark(slot, MarkStructural);
}
SceneDelta RenderScene::publish() {
    SceneDelta delta = std::move(pending_);
    pending_ = {};
    delta.base = published_;
    delta.revision = revision_;
    delta.topology = topology_;
    published_ = revision_;
    for (auto slot : delta.structural)
        marks_[slot] = 0;
    for (auto slot : delta.moved)
        marks_[slot] = 0;
    for (auto slot : delta.attributes)
        marks_[slot] = 0;
    return delta;
}
} // namespace afterlight
