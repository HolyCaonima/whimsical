#include "RenderTarget.h"
#include <stdexcept>

namespace whimsical {
Json textureContentsJson(const TextureContents& v) {
    return {{"rtVersion", std::to_string(v.rtVersion)},
            {"contentVersion", std::to_string(v.contentVersion)},
            {"renderFrame", std::to_string(v.renderFrame)},
            {"sourceTick", std::to_string(v.sourceTick)},
            {"width", v.width},
            {"height", v.height}};
}
bool RenderTargetResource::valid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return valid_;
}
void RenderTargetResource::invalidate() {
    std::lock_guard<std::mutex> lock(mutex_);
    valid_ = false;
}
void RenderTargetResource::publish(TextureContents value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (valid_)
        contents_ = value;
}
Json RenderTargetResource::info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto j = textureContentsJson(contents_);
    j["handle"] = handle;
    j["asset"] = asset->reference().json();
    j["assetGeneration"] = std::to_string(asset->generation);
    j["format"] = pixelFormatName(asset->format);
    j["bytesPerPixel"] = pixelBytes(asset->format);
    j["descriptorWidth"] = asset->width;
    j["descriptorHeight"] = asset->height;
    j["status"] = !valid_ ? "invalidated" : contents_.contentVersion ? "ready" : "uninitialized";
    return j;
}
bool PixelReadRequest::pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::Pending;
}
bool PixelReadRequest::claim() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::Pending)
        return false;
    state_ = State::Submitted;
    return true;
}
void PixelReadRequest::complete(PixelReadResult result) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::Submitted)
        return; // Cancellation wins over a GPU completion already in flight.
    result_ = std::move(result);
    state_ = State::Complete;
}
void PixelReadRequest::cancel(const std::string& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::Complete) {
        result_.status = status;
        state_ = State::Complete;
    }
}
std::optional<PixelReadResult> PixelReadRequest::take() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::Complete)
        return std::nullopt;
    return std::move(result_);
}
std::shared_ptr<RenderTargetResource>
RenderTargetAccess::acquire(std::shared_ptr<const RenderTargetAsset> a) {
    for (const auto& entry : targets_)
        if (entry.second->asset == a && entry.second->valid())
            return entry.second;
    auto rt = std::make_shared<RenderTargetResource>(std::to_string(++nextTarget_), std::move(a));
    targets_.emplace(rt->handle, rt);
    return rt;
}
std::shared_ptr<RenderTargetResource> RenderTargetAccess::target(const std::string& id) const {
    auto found = targets_.find(id);
    if (found == targets_.end())
        throw std::invalid_argument("Invalid RenderTarget handle: " + id);
    return found->second;
}
std::string RenderTargetAccess::read(const std::string& id, PixelRegion region, std::string version) {
    if (requests_.size() >= 64)
        throw std::runtime_error("At most 64 pixel reads may be outstanding; poll or cancel old requests");
    auto rt = target(id);
    auto r =
        std::make_shared<PixelReadRequest>(std::to_string(++nextRequest_), rt, region, std::move(version));
    if (!rt->valid())
        r->cancel("invalidated");
    requests_.emplace(r->handle, r);
    return r->handle;
}
std::shared_ptr<PixelReadRequest> RenderTargetAccess::request(const std::string& id) const {
    auto found = requests_.find(id);
    if (found == requests_.end())
        throw std::invalid_argument("Invalid pixel read ticket: " + id);
    return found->second;
}
void RenderTargetAccess::consumed(const std::string& id) {
    requests_.erase(id);
}
void RenderTargetAccess::discard(const std::string& id) {
    auto request = requests_.find(id);
    if (request != requests_.end()) {
        request->second->cancel("cancelled");
        requests_.erase(request);
    }
}
void RenderTargetAccess::release(const std::string& id) {
    auto rt = target(id);
    rt->invalidate();
    for (auto& entry : requests_)
        if (entry.second->target == rt)
            entry.second->cancel("invalidated");
    targets_.erase(id);
}
void RenderTargetAccess::reset() {
    for (auto& entry : targets_)
        entry.second->invalidate();
    for (auto& entry : requests_)
        entry.second->cancel("invalidated");
    targets_.clear();
    // Persistent application realms still own their tickets after scene replacement.
    // They can observe invalidated or cancel; retiring scene realms discard their own.
}
std::vector<std::shared_ptr<PixelReadRequest>> RenderTargetAccess::snapshot(uint64_t tick) {
    std::vector<std::shared_ptr<PixelReadRequest>> result;
    for (auto& entry : requests_) {
        auto& r = entry.second;
        if (!r->pending())
            continue;
        if (!r->published) {
            r->requestedTick = tick;
            r->published = true;
        }
        result.push_back(r);
    }
    return result;
}
} // namespace whimsical
