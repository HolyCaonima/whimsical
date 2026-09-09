#include "RenderGraph.h"
#include "renderCore/vulkan/GraphState.h"
#include "core/CpuProfile.h"
#include <algorithm>

namespace whimsical::rg {
AccessInfo accessInfo(Access access, Usage usage) {
    AccessInfo info;
    const bool reads = consumes(usage), writes = produces(usage);
    switch (access) {
    case Access::Compute:
    case Access::Graphics:
        info.stage = access == Access::Compute
                         ? VkPipelineStageFlags2(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)
                         : VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        info.layout = VK_IMAGE_LAYOUT_GENERAL;
        if (reads)
            info.access |= VK_ACCESS_2_SHADER_READ_BIT;
        if (writes)
            info.access |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        return info;
    case Access::Color:
        info.stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        info.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        info.access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        if (reads)
            info.access |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
        return info;
    case Access::Depth:
        // The depth test reads what the load op put there, so both bits belong to a
        // depth attachment however the contents got their value.
        info.stage =
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        info.layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        info.access =
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        return info;
    case Access::Transfer:
        info.stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        info.layout = writes ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        if (reads)
            info.access |= VK_ACCESS_2_TRANSFER_READ_BIT;
        if (writes)
            info.access |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
        return info;
    case Access::Vertex:
        info.stage = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;
        info.access = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
        return info;
    case Access::Index:
        info.stage = VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
        info.access = VK_ACCESS_2_INDEX_READ_BIT;
        return info;
    case Access::Build:
        info.stage = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        // Build inputs are read as buffers, a refit also reads the structure it updates.
        if (reads)
            info.access |= VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_SHADER_READ_BIT;
        if (writes)
            info.access |= VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        return info;
    case Access::Trace:
        info.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        info.access = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        return info;
    case Access::Present:
        // The presentation engine is ordered by the submit's semaphore, so the handover
        // needs the layout and an execution dependency, nothing more.
        info.layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        return info;
    case Access::Host:
        info.stage = VK_PIPELINE_STAGE_2_HOST_BIT;
        info.access = VK_ACCESS_2_HOST_READ_BIT;
        return info;
    }
    throw std::runtime_error("Unknown render graph access");
}

RenderGraph::RenderGraph(ResourcePool& pool) : impl_(std::make_unique<Impl>(*this, pool.registry(), &pool)) {}
RenderGraph::RenderGraph(const Registry& registry) : impl_(std::make_unique<Impl>(*this, registry, nullptr)) {}

void RenderGraph::Impl::reset() {
    declared_ = 0;
}

RenderGraph::Builder RenderGraph::add(std::string name) {
    if (impl_->declared_ == impl_->passes_.size())
        impl_->passes_.emplace_back();
    auto& pass = impl_->passes_[impl_->declared_];
    pass.uses.clear();
    pass.resolvedUses.clear();
    pass.shaders.clear();
    pass.mappings.clear();
    pass.bindings.clear();
    pass.colors.clear();
    pass.edges.clear();
    pass.name = std::move(name);
    pass.depth = {};
    pass.depthUse = 0;
    pass.depthClear = 1.f;
    pass.depthLoad = false;
    pass.program.reset();
    pass.elements = pass.localSize = {};
    pass.dispatchExtent = {};
    pass.sideEffect = false;
    pass.alive = true;
    pass.record = nullptr;
    return {this, impl_->declared_++};
}

RenderGraph::Builder& RenderGraph::Builder::use(ResourceRef ref, Access access, Usage usage) {
    graph_->impl_->passes_[pass_].uses.push_back({ref, access, usage});
    // Contents the pass reaches through this resource without having a name for them. The
    // declaration says so once, so being ordered against whatever produced them is not a
    // rule every ray query and every build has to remember.
    if (usage != Usage::Binding)
        for (auto reached : graph_->impl_->registry_[ref.id].reaches)
            graph_->impl_->passes_[pass_].uses.push_back({reached, access, Usage::Read});
    return *this;
}
RenderGraph::Builder& RenderGraph::Builder::read(ResourceRef ref, Access access) {
    return use(ref, access, Usage::Read);
}
RenderGraph::Builder& RenderGraph::Builder::read(const ResourceList& list, Access access) {
    for (auto ref : list)
        use(ref, access, Usage::Read);
    return *this;
}
RenderGraph::Builder& RenderGraph::Builder::overwrite(ResourceRef ref, Access access) {
    return use(ref, access, Usage::Overwrite);
}
RenderGraph::Builder& RenderGraph::Builder::overwrite(const ResourceList& list, Access access) {
    for (auto ref : list)
        use(ref, access, Usage::Overwrite);
    return *this;
}
RenderGraph::Builder& RenderGraph::Builder::modify(ResourceRef ref, Access access) {
    return use(ref, access, Usage::Modify);
}
RenderGraph::Builder& RenderGraph::Builder::modify(const ResourceList& list, Access access) {
    for (auto ref : list)
        use(ref, access, Usage::Modify);
    return *this;
}
RenderGraph::Builder& RenderGraph::Builder::color(ResourceId id) {
    auto& pass = graph_->impl_->passes_[pass_];
    pass.colors.push_back({id, uint32_t(pass.uses.size()), false, {}});
    return modify(id, Access::Color);
}
RenderGraph::Builder& RenderGraph::Builder::color(ResourceId id, ClearColor clear) {
    auto& pass = graph_->impl_->passes_[pass_];
    Impl::Attachment attachment;
    attachment.id = id;
    attachment.use = uint32_t(pass.uses.size());
    attachment.clear = true;
    std::copy(clear.begin(), clear.end(), attachment.value.color.float32);
    pass.colors.push_back(attachment);
    return overwrite(id, Access::Color);
}
RenderGraph::Builder& RenderGraph::Builder::depth(ResourceId id, std::optional<float> clear) {
    auto& pass = graph_->impl_->passes_[pass_];
    pass.depth = id;
    pass.depthClear = clear.value_or(1.f);
    pass.depthLoad = !clear.has_value();
    pass.depthUse = uint32_t(pass.uses.size());
    return clear ? overwrite(id, Access::Depth) : modify(id, Access::Depth);
}
RenderGraph::Builder& RenderGraph::Builder::shader(const std::vector<ShaderAccess>& accesses) {
    merge(graph_->impl_->passes_[pass_].shaders, accesses);
    return *this;
}
RenderGraph::Builder& RenderGraph::Builder::bind(ResourceRef shaderSlot, ResourceRef resource) {
    const auto& view = graph_->impl_->registry_.view(shaderSlot);
    if (!view)
        throw std::runtime_error("Shader slot has no descriptor view");
    auto& mappings = graph_->impl_->passes_[pass_].mappings;
    for (const auto& mapping : mappings)
        if (mapping.binding == view.binding)
            throw std::runtime_error("Shader slot mapped twice");
    mappings.push_back({view.binding, resource});
    return *this;
}

void RenderGraph::Impl::resolveShaders() {
    for (uint32_t p = 0; p < declared_; ++p) {
        auto& pass = passes_[p];
        pass.resolvedUses = pass.uses;
        pass.bindings.clear();
        for (const auto& shader : pass.shaders) {
            if (shader.binding >= registry_.bindingCount())
                throw std::runtime_error("Shader binding outside registry");
            const auto slot = registry_.binding(shader.binding);
            auto ref = slot;
            for (const auto& mapping : pass.mappings)
                if (mapping.binding == shader.binding)
                    ref = mapping.resource;
            if (!ref.id.valid() || ref.id.index >= registry_.size() ||
                (ref.slot == Slot::Previous && registry_[ref.id].lifetime != Lifetime::History))
                throw std::runtime_error("Invalid pass resource binding");
            const auto& expected = registry_.view(slot);
            const auto& actual = registry_.view(ref);
            if (registry_[slot.id].kind != registry_[ref.id].kind ||
                (actual && (expected.type != actual.type || expected.count != actual.count)) ||
                (expected.type == BindingType::StorageImage &&
                 registry_[slot.id].format != registry_[ref.id].format) ||
                (shader.writes && actual.readOnly))
                throw std::runtime_error(std::string(pass.name) +
                                         " has incompatible shader binding: " + registry_[ref.id].name);
            if (std::none_of(pass.bindings.begin(), pass.bindings.end(),
                             [&](const ShaderBinding& b) { return b.binding == shader.binding; }))
                pass.bindings.push_back({shader.binding, ref});
            bool declaredWrite = false;
            for (const auto& use : pass.uses) {
                if (!(use.ref == ref) || use.access != shader.access)
                    continue;
                if (produces(use.usage))
                    declaredWrite = true;
                if ((shader.writes && !produces(use.usage)) || (shader.reads && !consumes(use.usage)))
                    throw std::runtime_error(std::string(pass.name) +
                                             " contradicts shader access: " + registry_[ref.id].name);
            }
            if (shader.writes && !declaredWrite)
                throw std::runtime_error(std::string(pass.name) +
                                         " must declare overwrite or modify: " + registry_[ref.id].name);
            pass.resolvedUses.push_back({ref, shader.access, shader.reads ? Usage::Read : Usage::Binding});
            if (shader.reads || shader.writes)
                for (auto reached : registry_[ref.id].reaches)
                    pass.resolvedUses.push_back({reached, shader.access, Usage::Read});
        }
        for (const auto& mapping : pass.mappings)
            if (std::none_of(pass.bindings.begin(), pass.bindings.end(),
                             [&](const ShaderBinding& b) { return b.binding == mapping.binding; }))
                throw std::runtime_error(std::string(pass.name) + " maps a slot unused by its shaders");
    }
}

RenderGraph::Builder& RenderGraph::Builder::dispatch(const Program& program, Extent3D elements) {
    graph_->impl_->passes_[pass_].program = program.storage;
    graph_->impl_->passes_[pass_].elements = elements;
    graph_->impl_->passes_[pass_].localSize = program.localSize;
    graph_->impl_->passes_[pass_].dispatchExtent = {};
    return shader(program.accesses);
}
RenderGraph::Builder& RenderGraph::Builder::dispatch(const Program& program, ResourceId extentOf) {
    dispatch(program, Extent3D{});
    if (!extentOf.valid() || extentOf.index >= graph_->impl_->registry_.size() ||
        graph_->impl_->registry_[extentOf].kind != Kind::Image)
        throw std::invalid_argument("Dispatch extent must name an image resource");
    graph_->impl_->passes_[pass_].dispatchExtent = extentOf;
    return *this;
}
RenderGraph::Builder& RenderGraph::Builder::record(std::function<void(const PassContext&)> body) {
    graph_->impl_->passes_[pass_].record = std::move(body);
    return *this;
}
RenderGraph::Builder& RenderGraph::Builder::sideEffect() {
    graph_->impl_->passes_[pass_].sideEffect = true;
    return *this;
}

// Every history half is its own content slot, so the version a pass consumes is never
// confused with the one another pass is producing into the other half of the pair. Walking
// the declarations forward turns each consuming use into an edge to the pass that produced
// the version it sees; a producing use then becomes the version the rest of the frame
// consumes. This is the whole of the dependency analysis.
void RenderGraph::Impl::analyse() {
    producer_.assign(registry_.size() * 2, NoPass);
    for (uint32_t p = 0; p < declared_; ++p) {
        auto& pass = passes_[p];
        pass.edges.clear();
        for (auto& use : pass.resolvedUses) {
            use.source = consumes(use.usage) ? producer_[content(use.ref)] : NoPass;
            if (use.source != NoPass &&
                std::find(pass.edges.begin(), pass.edges.end(), use.source) == pass.edges.end())
                pass.edges.push_back(use.source);
        }
        for (const auto& use : pass.resolvedUses)
            if (produces(use.usage))
                producer_[content(use.ref)] = p;
    }
}

// A pass survives if it produced a version something still consumes. The roots are the last
// producer of contents that outlive the frame, plus the passes that admit to changing state
// the graph cannot see. Edges only ever point backwards, so one reverse sweep is the whole
// reachability. Conditional features therefore cost nothing when their consumer is absent,
// and a version that is replaced before anyone reads it takes its producer with it.
void RenderGraph::Impl::cull() {
    for (uint32_t p = 0; p < declared_; ++p)
        passes_[p].alive = passes_[p].sideEffect;
    for (uint32_t slot = 0; slot < producer_.size(); ++slot) {
        const auto& resource = registry_[ResourceId{uint16_t(slot / 2)}];
        if (producer_[slot] != NoPass && (crossesFrames(resource.lifetime) || resource.handover))
            passes_[producer_[slot]].alive = true;
    }
    live_ = 0;
    for (uint32_t p = declared_; p-- > 0;) {
        if (!passes_[p].alive)
            continue;
        ++live_;
        for (auto edge : passes_[p].edges)
            passes_[edge].alive = true;
    }
}

// The one thing the declarations can be wrong about that nothing downstream would notice:
// consuming contents that do not exist. A transient starts every frame undefined, so a read
// or a loaded attachment with no producer would be reading whatever the storage it now
// shares was last used for.
void RenderGraph::Impl::validate() const {
    for (uint32_t p = 0; p < declared_; ++p) {
        const auto& pass = passes_[p];
        if (!pass.alive)
            continue;
        for (const auto& use : pass.resolvedUses) {
            const auto& declaration = registry_[use.ref.id];
            if (consumes(use.usage) && use.source == NoPass && !crossesFrames(declaration.lifetime))
                throw std::runtime_error(std::string(pass.name) + " consumes '" + declaration.name +
                                         "', which nothing in this frame produced");
        }
    }
}

// Walking the live passes backwards says, for every version produced, whether anything
// consumes it before it is replaced. Contents that cross the frame boundary start out
// consumed because the next frame is the reader. Attachment store ops come straight from
// this, so "nobody reads it" is stated once instead of hand-written per pass.
void RenderGraph::Impl::liveness() {
    std::vector<uint8_t> consumed(registry_.size() * 2, 0);
    for (uint16_t i = 0; i < registry_.size(); ++i)
        if (crossesFrames(registry_[{i}].lifetime) || registry_[{i}].handover)
            consumed[i * 2] = consumed[i * 2 + 1] = 1;
    for (uint32_t p = declared_; p-- > 0;) {
        auto& pass = passes_[p];
        if (!pass.alive)
            continue;
        for (auto& use : pass.resolvedUses)
            if (produces(use.usage))
                use.consumedLater = consumed[content(use.ref)] != 0;
        for (const auto& use : pass.resolvedUses)
            if (produces(use.usage) && !consumes(use.usage))
                consumed[content(use.ref)] = 0;
        for (const auto& use : pass.resolvedUses)
            if (consumes(use.usage))
                consumed[content(use.ref)] = 1;
    }
}

// Storage follows live content and binding uses. Merely declaring a shader view does
// not make a resource reachable; reflection identifies the slots each live pass uses.
void RenderGraph::Impl::plan(uint32_t width, uint32_t height) {
    struct Range {
        ResourceId id;
        uint32_t first = NoPass, last = 0;
    };
    std::vector<Range> ranges(registry_.size());
    for (uint16_t i = 0; i < registry_.size(); ++i)
        ranges[i].id = ResourceId{i};
    for (uint32_t p = 0; p < declared_; ++p) {
        if (!passes_[p].alive)
            continue;
        for (const auto& use : passes_[p].resolvedUses) {
            auto& range = ranges[use.ref.id.index];
            range.first = std::min(range.first, p);
            range.last = std::max(range.last, p);
        }
    }
    // A host/presentation handover is a consumer after all passes. Its output must
    // neither be culled nor have its storage recycled before that consumer sees it.
    for (auto& range : ranges)
        if (registry_[range.id].handover)
            range.last = declared_;
    touched_.assign(registry_.size(), 0);
    residency_.assign(registry_.size(), {});
    for (uint16_t i = 0; i < registry_.size(); ++i) {
        const auto& declaration = registry_[{i}];
        touched_[i] = ranges[i].first != NoPass;
        residency_[i].root = ResourceId{i};
        residency_[i].needed =
            graphOwned(declaration.lifetime) && (crossesFrames(declaration.lifetime) || touched_[i]);
    }
    aliased_ = 0;
    // One tenant list per storage signature; a transient joins the first tenant that is
    // already dead when it is first written.
    struct Tenant {
        uint64_t signature;
        uint32_t last;
        ResourceId root;
    };
    std::vector<Tenant> tenants;
    std::vector<Range> ordered;
    for (const auto& range : ranges)
        if (range.first != NoPass && registry_[range.id].lifetime == Lifetime::Transient)
            ordered.push_back(range);
    std::sort(ordered.begin(), ordered.end(),
              [](const Range& a, const Range& b) { return a.first < b.first; });
    for (const auto& range : ordered) {
        const uint64_t signature = storageSignature(registry_[range.id], width, height);
        auto tenant = std::find_if(tenants.begin(), tenants.end(), [&](const Tenant& t) {
            return t.signature == signature && t.last < range.first;
        });
        if (tenant == tenants.end())
            tenants.push_back({signature, range.last, range.id});
        else {
            residency_[range.id.index].root = tenant->root;
            tenant->last = range.last;
            ++aliased_;
        }
    }
}

void RenderGraph::Impl::compile(uint32_t width, uint32_t height) {
    CpuScope scope("Graph / Compile");
    resolveShaders();
    analyse();
    cull();
    validate();
    liveness();
    plan(width, height);
    if (pool_)
        pool_->realize(width, height, residency_);
}

void RenderGraph::Impl::checkDeclared(uint32_t pass, ResourceRef ref) const {
    const auto& declaring = passes_[pass];
    for (const auto& use : declaring.resolvedUses)
        if (use.ref == ref)
            return;
    throw std::runtime_error(std::string(declaring.name) + " touched '" + registry_[ref.id].name +
                             "' without declaring it");
}

const Declaration& PassContext::declaration(ResourceId id) const { return pool->declaration(id); }
Image& PassContext::image(ResourceRef ref) const {
    graph->checkDeclared(pass, ref);
    return pool->image(ref);
}
Buffer& PassContext::buffer(ResourceRef ref) const {
    graph->checkDeclared(pass, ref);
    return pool->buffer(ref);
}

// One barrier batch, derived from what every physical slot was last used for. The state
// lives in the pool and survives the frame boundary, so the first access of a ping-ponged
// history half is ordered against last frame's reads without any extra rule.
void RenderGraph::Impl::synchronise(VkCommandBuffer command, const Use* uses, uint32_t count) {
    merged_.clear();
    for (uint32_t i = 0; i < count; ++i) {
        const auto& use = uses[i];
        if (registry_[use.ref.id].lifetime == Lifetime::External)
            continue;
        const uint32_t slot = pool_->physical(use.ref);
        const auto info = accessInfo(use.access, use.usage);
        auto found = std::find_if(merged_.begin(), merged_.end(),
                                  [&](const Merged& other) { return other.slot == slot; });
        if (found == merged_.end()) {
            merged_.push_back({use.ref, slot, info, produces(use.usage)});
            continue;
        }
        if (info.layout != VK_IMAGE_LAYOUT_UNDEFINED) {
            if (found->info.layout != VK_IMAGE_LAYOUT_UNDEFINED && found->info.layout != info.layout)
                throw std::runtime_error("A pass asked one resource for two image layouts: " +
                                         registry_[use.ref.id].name);
            found->info.layout = info.layout;
        }
        found->info.stage |= info.stage;
        found->info.access |= info.access;
        found->produces = found->produces || produces(use.usage);
    }
    imageBarriers_.clear();
    bufferBarriers_.clear();
    memoryBarriers_.clear();
    for (const auto& use : merged_) {
        const auto& declaration = registry_[use.ref.id];
        const auto& info = use.info;
        auto& state = pool_->state(use.slot);
        const bool isImage = declaration.kind == Kind::Image;
        Image* image = isImage ? &pool_->image(use.ref) : nullptr;
        const bool transition = isImage && image->layout != info.layout;
        const bool write = use.produces || transition;
        VkPipelineStageFlags2 sourceStage = 0;
        VkAccessFlags2 sourceAccess = 0;
        if (write) {
            sourceStage = state.writeStage | state.readStages;
            sourceAccess = state.writeAccess;
        } else {
            if (!state.writeStage ||
                (!(info.stage & ~state.flushedStages) && !(info.access & ~state.flushedAccess))) {
                state.readStages |= info.stage;
                continue;
            }
            sourceStage = state.writeStage;
            sourceAccess = state.writeAccess;
        }
        if (isImage) {
            VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            barrier.srcStageMask = sourceStage ? sourceStage : VK_PIPELINE_STAGE_2_NONE;
            barrier.srcAccessMask = sourceAccess;
            barrier.dstStageMask = info.stage;
            barrier.dstAccessMask = info.access;
            barrier.oldLayout = image->layout;
            barrier.newLayout = info.layout;
            barrier.image = image->handle;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.subresourceRange = {VkImageAspectFlags(declaration.format == Format::D32
                                                               ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                               : VK_IMAGE_ASPECT_COLOR_BIT),
                                        0, image->mipLevels, 0, 1};
            imageBarriers_.push_back(barrier);
            image->layout = info.layout;
        } else if (declaration.kind == Kind::Buffer) {
            const auto& buffer = pool_->buffer(use.ref);
            VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            barrier.srcStageMask = sourceStage ? sourceStage : VK_PIPELINE_STAGE_2_NONE;
            barrier.srcAccessMask = sourceAccess;
            barrier.dstStageMask = info.stage;
            barrier.dstAccessMask = info.access;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = buffer.handle;
            barrier.offset = 0;
            barrier.size = VK_WHOLE_SIZE;
            bufferBarriers_.push_back(barrier);
        } else {
            // Acceleration structures are reached through device addresses rather than one
            // handle a barrier could name, so their dependency is a memory one. A memory
            // barrier has no subject, so a batch never needs more than the union of them.
            if (memoryBarriers_.empty())
                memoryBarriers_.push_back({VK_STRUCTURE_TYPE_MEMORY_BARRIER_2});
            auto& barrier = memoryBarriers_.front();
            barrier.srcStageMask |= sourceStage;
            barrier.srcAccessMask |= sourceAccess;
            barrier.dstStageMask |= info.stage;
            barrier.dstAccessMask |= info.access;
        }
        if (write) {
            state.writeStage = info.stage;
            state.writeAccess = info.access;
            state.readStages = use.produces ? 0 : info.stage;
            state.flushedStages = 0;
            state.flushedAccess = 0;
        } else {
            state.readStages |= info.stage;
            state.flushedStages |= info.stage;
            state.flushedAccess |= info.access;
        }
    }
    if (imageBarriers_.empty() && bufferBarriers_.empty() && memoryBarriers_.empty())
        return;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = uint32_t(memoryBarriers_.size());
    dependency.pMemoryBarriers = memoryBarriers_.data();
    dependency.bufferMemoryBarrierCount = uint32_t(bufferBarriers_.size());
    dependency.pBufferMemoryBarriers = bufferBarriers_.data();
    dependency.imageMemoryBarrierCount = uint32_t(imageBarriers_.size());
    dependency.pImageMemoryBarriers = imageBarriers_.data();
    vkCmdPipelineBarrier2(command, &dependency);
    barriers_ += uint32_t(imageBarriers_.size() + bufferBarriers_.size() + memoryBarriers_.size());
}

// A resource leaves the graph in the state its consumer needs: the swapchain in PRESENT_SRC
// for the presentation engine, a readback buffer made visible to the host, whose fence wait
// is an execution dependency and not a memory one. Deriving that from the declaration is
// what removes the pass that used to exist only to make the transition.
void RenderGraph::Impl::handover(VkCommandBuffer command) {
    handover_.clear();
    for (uint16_t i = 0; i < registry_.size(); ++i) {
        const auto& declaration = registry_[{i}];
        if (declaration.handover && touched_[i])
            handover_.push_back({ResourceId{i}, *declaration.handover, Usage::Read});
    }
    if (!handover_.empty())
        synchronise(command, handover_.data(), uint32_t(handover_.size()));
}

RenderGraph::Builder& RenderGraph::Builder::constants(std::vector<uint8_t> bytes) {
    if (bytes.size() % 4 || bytes.size() > graph_->impl_->registry_.pushConstantBytes())
        throw std::invalid_argument("Pass constants must fit the registry's aligned push constant range");
    graph_->impl_->passes_[pass_].constants = std::move(bytes);
    return *this;
}

void RenderGraph::Impl::execute(VkCommandBuffer command, GpuProfiler& profiler) {
    PassContext context;
    context.command = command;
    context.layout = pool_->pipelineLayout();
    context.pool = pool_;
    context.profiler_ = &profiler;
    context.graph = &owner;
    barriers_ = 0;
    for (uint32_t p = 0; p < declared_; ++p) {
        const auto& pass = passes_[p];
        if (!pass.alive)
            continue;
        CpuScope cpuScope(pass.name.c_str());
        GpuScope gpuScope(profiler, command, pass.name.c_str());
        context.descriptors = pass.bindings.empty() ? VK_NULL_HANDLE : pool_->descriptors(p, pass.bindings);
        synchronise(command, pass.resolvedUses.data(), uint32_t(pass.resolvedUses.size()));
        const bool renders = !pass.colors.empty() || pass.depth.valid();
        uint32_t width = pool_->width(), height = pool_->height();
        if (renders) {
            const auto& reference = pass.colors.empty() ? pass.depth : pass.colors.front().id;
            width = pool_->extentWidth(reference);
            height = pool_->extentHeight(reference);
        }
        context.width = width;
        context.height = height;
        context.pass = p;
        auto& attachments = attachments_;
        VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (renders) {
            attachments.clear();
            for (const auto& attachment : pass.colors) {
                VkRenderingAttachmentInfo info{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                info.imageView = pool_->image(attachment.id).view;
                info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                info.loadOp = attachment.clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
                info.storeOp = pass.resolvedUses[attachment.use].consumedLater
                                   ? VK_ATTACHMENT_STORE_OP_STORE
                                   : VK_ATTACHMENT_STORE_OP_DONT_CARE;
                info.clearValue = attachment.value;
                attachments.push_back(info);
            }
            VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
            rendering.renderArea = {{0, 0}, {width, height}};
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = uint32_t(attachments.size());
            rendering.pColorAttachments = attachments.data();
            if (pass.depth.valid()) {
                depthAttachment.imageView = pool_->image(pass.depth).view;
                depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                depthAttachment.loadOp =
                    pass.depthLoad ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
                depthAttachment.storeOp = pass.resolvedUses[pass.depthUse].consumedLater
                                              ? VK_ATTACHMENT_STORE_OP_STORE
                                              : VK_ATTACHMENT_STORE_OP_DONT_CARE;
                depthAttachment.clearValue.depthStencil = {pass.depthClear, 0};
                rendering.pDepthAttachment = &depthAttachment;
            }
            vkCmdBeginRendering(command, &rendering);
            VkViewport viewport{0, 0, float(width), float(height), 0, 1};
            VkRect2D scissor{{0, 0}, {width, height}};
            vkCmdSetViewport(command, 0, 1, &viewport);
            vkCmdSetScissor(command, 0, 1, &scissor);
        }
        if (!pass.constants.empty())
            vkCmdPushConstants(command, context.layout, VK_SHADER_STAGE_ALL, 0,
                               uint32_t(pass.constants.size()), pass.constants.data());
        if (pass.program) {
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pass.program->pipeline);
            if (!pass.bindings.empty())
                vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, context.layout, 0, 1,
                                        &context.descriptors, 0, nullptr);
            auto size = pass.elements;
            if (pass.dispatchExtent.valid())
                size = {pool_->extentWidth(pass.dispatchExtent), pool_->extentHeight(pass.dispatchExtent), 1};
            const auto& local = pass.localSize;
            vkCmdDispatch(command, (size.x + local.x - 1) / local.x,
                          (size.y + local.y - 1) / local.y, (size.z + local.z - 1) / local.z);
        }
        if (pass.record)
            pass.record(context);
        if (renders)
            vkCmdEndRendering(command);
    }
    handover(command);
}
} // namespace whimsical::rg

namespace whimsical::rg {
RenderGraph::~RenderGraph() = default;
void RenderGraph::reset() { impl_->reset(); }
void RenderGraph::compile(uint32_t width, uint32_t height) { impl_->compile(width, height); }
uint32_t RenderGraph::passCount() const { return impl_->declared_; }
uint32_t RenderGraph::livePasses() const { return impl_->live_; }
uint32_t RenderGraph::barrierCount() const { return impl_->barriers_; }
uint32_t RenderGraph::aliasedResources() const { return impl_->aliased_; }
bool RenderGraph::alive(uint32_t pass) const { return impl_->passes_[pass].alive; }
const std::vector<Residency>& RenderGraph::residency() const { return impl_->residency_; }
const std::vector<ShaderBinding>& RenderGraph::bindings(uint32_t pass) const { return impl_->passes_[pass].bindings; }
void RenderGraph::checkDeclared(uint32_t pass, ResourceRef ref) const { impl_->checkDeclared(pass, ref); }
void GraphAccess::execute(RenderGraph& graph, VkCommandBuffer command, GpuProfiler& profiler) {
    graph.impl_->execute(command, profiler);
}
} // namespace whimsical::rg
