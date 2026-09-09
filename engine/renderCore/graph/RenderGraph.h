#pragma once
#include "Registry.h"
#include "ShaderAccess.h"
#include <array>
#include <functional>
#include <memory>
#include <optional>

namespace whimsical::rg {
class ResourcePool;
struct PassContext;
struct GraphAccess;
struct Residency;
using ClearColor = std::array<float, 4>;

// GPU work declarations shared by all systems. This interface has no Vulkan, World,
// rendering algorithm, window or frame clock. Native recording is a backend extension.
class RenderGraph {
  public:
    class Builder {
        RenderGraph* graph_;
        uint32_t pass_;
        friend class RenderGraph;
        Builder(RenderGraph* graph, uint32_t pass) : graph_(graph), pass_(pass) {}

      public:
        Builder& use(ResourceRef, Access, Usage);
        Builder& read(ResourceRef, Access);
        Builder& read(const ResourceList&, Access);
        Builder& overwrite(ResourceRef, Access);
        Builder& overwrite(const ResourceList&, Access);
        Builder& modify(ResourceRef, Access);
        Builder& modify(const ResourceList&, Access);
        // Attachment load/clear and dependencies are one declaration.
        Builder& color(ResourceId);
        Builder& color(ResourceId, ClearColor clear);
        Builder& depth(ResourceId, std::optional<float> clear = 1.f);
        Builder& shader(const std::vector<ShaderAccess>&);
        // Shader slots and concrete resources are distinct. Reflection validates bindings;
        // overwrite/modify explicitly states whether the pass preserves old contents.
        Builder& bind(ResourceRef shaderSlot, ResourceRef resource);
        Builder& dispatch(const Program&, Extent3D elements);
        Builder& dispatch(const Program&, ResourceId extentOf);
        // Native backends opt into renderCore/vulkan/GraphAccess.h for PassContext.
        Builder& record(std::function<void(const PassContext&)>);
        Builder& sideEffect(); // effects on state the graph does not own
    };

    explicit RenderGraph(ResourcePool&);
    explicit RenderGraph(const Registry&); // declaration-only compilation, without a device
    ~RenderGraph();
    RenderGraph(const RenderGraph&) = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;
    void reset();
    Builder add(std::string name);
    void compile(uint32_t referenceWidth = 1, uint32_t referenceHeight = 1);
    uint32_t passCount() const;
    uint32_t livePasses() const;
    uint32_t barrierCount() const;
    uint32_t aliasedResources() const;
    bool alive(uint32_t pass) const;
    const std::vector<Residency>& residency() const;
    const std::vector<ShaderBinding>& bindings(uint32_t pass) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend struct GraphAccess;
    friend struct PassContext;
    void checkDeclared(uint32_t pass, ResourceRef) const;
};
} // namespace whimsical::rg