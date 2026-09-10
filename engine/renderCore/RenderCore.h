#pragma once
#include "graph/Registry.h"
#include "core/GpuProfile.h"
#include <memory>
#include <optional>

namespace whimsical {
class GpuProfiler;
namespace rg {
class RenderGraph;
struct Program;
} // namespace rg
namespace rc {
enum class QueueClass { General, Compute };
struct VulkanAccess;
class NativeResources;

struct DeviceOptions {
#ifdef NDEBUG
    bool validation = false;
#else
    bool validation = true;
#endif
    bool rayQueries = false;
    void* presentationWindow = nullptr; // optional native window; null supports offscreen compute
};

// Owned by the application, not a rendering system. All GPU systems use this device;
// their execution contexts have independent resources, submissions and history clocks.
// Each context has one owner thread; separate contexts may run concurrently.
// Device-level caches, queue access and profiling are synchronized by RenderCore.
class RenderCore {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend struct VulkanAccess;
    friend class GraphContext;

  public:
    explicit RenderCore(const DeviceOptions& = {});
    ~RenderCore();
    RenderCore(const RenderCore&) = delete;
    RenderCore& operator=(const RenderCore&) = delete;
    void waitIdle();
    uint32_t errors() const;
    // Capture submissions from every execution context over a wall-clock window.
    // Results contain summed GPU intervals, not CPU waits or elapsed frame time.
    void requestProfile(uint64_t request, double windowMilliseconds = 1000);
    void endProfile(); // close the window early, e.g. when the GPU owner shuts down
    std::optional<GpuProfile> takeProfile();
};

struct ProfileRequest {
    uint64_t request = 0;
    uint64_t sequence = 0;          // supplied by the owner: simulation tick, view frame, bake, ...
    uint32_t width = 0, height = 0; // optional diagnostic context
};

// An execution scope for one system or a group of cooperating systems. The registry
// describes its resource nodes; every contributor fills the SAME RenderGraph and passes
// ResourceRef outputs to consumers. No World, camera, viewport or rendering pipeline.
// Registry and RenderCore outlive this scope. ResourceIds are local to this registry.
class GraphContext {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend struct VulkanAccess;
    friend class NativeResources;

  public:
    GraphContext(RenderCore&, const rg::Registry&, std::string name = "Graph",
                 QueueClass queue = QueueClass::General);
    ~GraphContext();
    GraphContext(const GraphContext&) = delete;
    GraphContext& operator=(const GraphContext&) = delete;
    rg::RenderGraph& graph();
    const rg::Program& compute(const std::vector<uint32_t>& spirv);
    const rg::Program& compute(const std::string& name, const std::string& source);
    // Owned CPU bytes become transfer nodes; callers never manage staging lifetimes.
    // Buffer payloads are complete, four-byte-aligned shader storage values.
    void upload(rg::ResourceRef, std::vector<uint8_t> bytes);
    // A partial update preserves all bytes outside the supplied range.
    void uploadRange(rg::ResourceRef, uint64_t offset, std::vector<uint8_t> bytes);
    void copyBuffer(rg::ResourceRef source, rg::ResourceRef destination, uint64_t sourceOffset,
                    uint64_t destinationOffset, uint64_t bytes);
    void copyBuffer(rg::ResourceRef source, rg::ResourceRef destination);
    // destination is a buffer declared with handover=Access::Host.
    void readback(rg::ResourceRef source, rg::ResourceId destination);
    std::vector<uint8_t> readbackData(rg::ResourceId) const;

    void compile(uint32_t referenceWidth = 1, uint32_t referenceHeight = 1);
    void record(const ProfileRequest& = {});
    void submit();
    bool poll(); // true once this context's last submission has completed
    void wait();
    // History advances only when its owner asks; submit/present never flips it.
    void advanceHistory();
    double gpuMilliseconds() const;
    std::optional<GpuProfile> takeProfile();
};
} // namespace rc
} // namespace whimsical
