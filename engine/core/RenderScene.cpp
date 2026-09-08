#include "RenderScene.h"
#include "assets/MaterialAsset.h"
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
uint32_t RenderScene::retainMaterial(const std::shared_ptr<const MaterialAsset>& asset) {
    for (uint32_t i = 0; i < materialSlots_.size(); ++i)
        if (materialSlots_[i].asset == asset) {
            ++materialSlots_[i].users;
            return i;
        }
    materialSlots_.push_back({asset, 1});
    materials_.push_back(asset->parameters);
    return uint32_t(materials_.size() - 1);
}
void RenderScene::releaseMaterial(uint32_t index) {
    if (--materialSlots_[index].users)
        return;
    // Compact only when an asset loses its last proxy. Unused assets must not
    // retain shaders/textures or consume the renderer's binding limits.
    const auto last = uint32_t(materialSlots_.size() - 1);
    if (index != last) {
        materialSlots_[index] = std::move(materialSlots_.back());
        materials_[index] = std::move(materials_.back());
        for (uint32_t slot = 0; slot < proxies_.size(); ++slot)
            if (proxies_[slot].live && proxies_[slot].attributes.material == last) {
                proxies_[slot].attributes.material = index;
                mark(slot, MarkAttributes);
            }
    }
    materialSlots_.pop_back();
    materials_.pop_back();
}
uint32_t RenderScene::create(const ProxyTransform& transform, ProxyAttributes attributes,
                             const std::shared_ptr<const MaterialAsset>& material) {
    attributes.material = retainMaterial(material);
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
    releaseMaterial(p.attributes.material);
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
void RenderScene::setAttributes(uint32_t slot, ProxyAttributes attributes,
                                const std::shared_ptr<const MaterialAsset>& material) {
    if (!proxy(slot).live)
        return;
    auto previous = proxies_[slot].attributes.material;
    attributes.material = previous;
    const bool materialChanged = materialSlots_[previous].asset != material;
    if (materialChanged)
        attributes.material = retainMaterial(material);
    if (proxies_[slot].attributes == attributes && !materialChanged)
        return;
    // Geometry is the one attribute a consumer cannot fold into an incremental update.
    if (proxies_[slot].attributes.shape != attributes.shape)
        ++topology_;
    proxies_[slot].attributes = attributes;
    if (materialChanged)
        releaseMaterial(previous);
    ++revision_;
    mark(slot, MarkAttributes);
}
void RenderScene::setVisible(uint32_t slot, bool visible) {
    if (!proxy(slot).live)
        return;
    auto attributes = proxy(slot).attributes;
    attributes.visible = visible;
    setAttributes(slot, attributes, materialSlots_[attributes.material].asset);
}
void RenderScene::geometryChanged(uint32_t slot) {
    if (!proxy(slot).live)
        return;
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
void RenderScene::exchangeScene(RenderScene& other) {
    proxies_.swap(other.proxies_);
    free_.swap(other.free_);
    materialSlots_.swap(other.materialSlots_);
    materials_.swap(other.materials_);
    marks_.assign(proxies_.size(), 0);
    pending_ = {};
    revision_ += other.revision_ + 1;
    topology_ += other.topology_ + 1;
    for (uint32_t slot = 0; slot < proxies_.size(); ++slot)
        mark(slot, MarkStructural);
    other.marks_.assign(other.proxies_.size(), 0);
    other.pending_ = {};
}
} // namespace afterlight
