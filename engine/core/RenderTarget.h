#pragma once
#include "assets/RenderTargetAsset.h"
#include <map>
#include <mutex>
#include <optional>

namespace whimsical {
// Version of completed GPU contents, delivered without exposing a mapped GPU pointer.
struct TextureContents {
    uint64_t rtVersion = 0, contentVersion = 0, renderFrame = 0, sourceTick = 0;
    uint32_t width = 0, height = 0;
};
class RenderTargetResource {
    mutable std::mutex mutex_;
    bool valid_ = true;
    TextureContents contents_;

  public:
    const std::string handle;
    const std::shared_ptr<const RenderTargetAsset> asset;
    RenderTargetResource(std::string id, std::shared_ptr<const RenderTargetAsset> a)
        : handle(std::move(id)), asset(std::move(a)) {}
    bool valid() const;
    void invalidate();
    void publish(TextureContents);
    Json info() const;
};
struct PixelRegion {
    uint32_t x = 0, y = 0, width = 0, height = 0; // Zero width/height: remaining extent.
};
struct PixelReadResult {
    std::string status;
    TextureContents contents;
    PixelRegion region;
    std::vector<uint8_t> bytes;
};
class PixelReadRequest {
    mutable std::mutex mutex_;
    enum class State { Pending, Submitted, Complete };
    State state_ = State::Pending;
    PixelReadResult result_;

  public:
    const std::string handle;
    const std::shared_ptr<RenderTargetResource> target;
    const PixelRegion region;
    const std::string expectedVersion;
    uint64_t requestedTick = 0; // Set by the producer before the first publication.
    bool published = false;     // Producer-only; survives dropped snapshots.
    PixelReadRequest(std::string id, std::shared_ptr<RenderTargetResource> rt, PixelRegion r,
                     std::string version)
        : handle(std::move(id)), target(std::move(rt)), region(r), expectedVersion(std::move(version)) {}
    bool pending() const;
    bool claim(); // Exactly once, even if a snapshot is presented repeatedly.
    void complete(PixelReadResult);
    void cancel(const std::string& status);
    std::optional<PixelReadResult> take();
};
// Simulation-owned handle table. Only the resource/request mailboxes above cross threads.
// Requests remain in every snapshot until claimed; the latest-wins FrameMailbox loses no work.
class RenderTargetAccess {
    uint64_t nextTarget_ = 0, nextRequest_ = 0;
    std::map<std::string, std::shared_ptr<RenderTargetResource>> targets_;
    std::map<std::string, std::shared_ptr<PixelReadRequest>> requests_;

  public:
    ~RenderTargetAccess() {
        reset();
    }
    std::shared_ptr<RenderTargetResource> acquire(std::shared_ptr<const RenderTargetAsset>);
    std::shared_ptr<RenderTargetResource> target(const std::string&) const;
    std::string read(const std::string&, PixelRegion, std::string expectedVersion = {});
    std::shared_ptr<PixelReadRequest> request(const std::string&) const;
    void consumed(const std::string&);
    void discard(const std::string&); // Realm teardown relinquishes any still-owned ticket.
    void release(const std::string&);
    void reset();
    std::vector<std::shared_ptr<PixelReadRequest>> snapshot(uint64_t tick);
};
Json textureContentsJson(const TextureContents&);
} // namespace whimsical
