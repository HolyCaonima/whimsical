#include "RenderGraph.h"
#include "core/CpuProfile.h"
#include <algorithm>

namespace afterlight::rg {
AccessInfo accessInfo(Access access) {
    AccessInfo info;
    auto layout = [&](VkImageLayout wanted) {
        if (info.layout != VK_IMAGE_LAYOUT_UNDEFINED && info.layout != wanted)
            throw std::runtime_error("A pass asked one resource for two image layouts");
        info.layout = wanted;
    };
    if (has(access, Access::ComputeRead)) {
        info.stage |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        info.access |= VK_ACCESS_2_SHADER_READ_BIT;
        layout(VK_IMAGE_LAYOUT_GENERAL);
    }
    if (has(access, Access::ComputeWrite)) {
        info.stage |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        info.access |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        layout(VK_IMAGE_LAYOUT_GENERAL);
    }
    if (has(access, Access::GraphicsRead)) {
        info.stage |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        info.access |= VK_ACCESS_2_SHADER_READ_BIT;
        layout(VK_IMAGE_LAYOUT_GENERAL);
    }
    if (has(access, Access::ColorWrite)) {
        info.stage |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        info.access |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
        layout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
    if (has(access, Access::DepthWrite)) {
        info.stage |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        info.access |=
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        layout(VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    }
    if (has(access, Access::TransferRead)) {
        info.stage |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        info.access |= VK_ACCESS_2_TRANSFER_READ_BIT;
        layout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
    if (has(access, Access::TransferWrite)) {
        info.stage |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        info.access |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
        layout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    }
    if (has(access, Access::VertexBuffer)) {
        info.stage |= VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;
        info.access |= VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
    }
    if (has(access, Access::IndexBuffer)) {
        info.stage |= VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
        info.access |= VK_ACCESS_2_INDEX_READ_BIT;
    }
    if (has(access, Access::BuildRead)) {
        info.stage |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        info.access |= VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_SHADER_READ_BIT;
    }
    if (has(access, Access::BuildWrite)) {
        info.stage |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        info.access |= VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
                       VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    }
    if (has(access, Access::TraceRead)) {
        info.stage |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        info.access |= VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    }
    if (has(access, Access::Present))
        layout(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    return info;
}

RenderGraph::RenderGraph(ResourcePool& pool) : pool_(pool) {
    aliasRoot_.resize(pool_.resourceCount());
}

void RenderGraph::reset() {
    passes_.clear();
    uses_.clear();
    colors_.clear();
}

RenderGraph::Builder RenderGraph::add(const char* name) {
    Pass pass;
    pass.name = name;
    pass.first = uint32_t(uses_.size());
    pass.firstColor = uint32_t(colors_.size());
    passes_.push_back(std::move(pass));
    return {this, uint32_t(passes_.size() - 1)};
}

RenderGraph::Builder& RenderGraph::Builder::read(ResourceRef ref, Access access) {
    graph_->uses_.push_back({ref, access});
    graph_->passes_[pass_].count++;
    return *this;
}
RenderGraph::Builder& RenderGraph::Builder::read(const ResourceList& list, Access access) {
    for (auto ref : list)
        read(ref, access);
    return *this;
}
RenderGraph::Builder& RenderGraph::Builder::write(ResourceRef ref, Access access) {
    return read(ref, access);
}
RenderGraph::Builder& RenderGraph::Builder::write(const ResourceList& list, Access access) {
    return read(list, access);
}
RenderGraph::Builder& RenderGraph::Builder::color(ResourceId id) {
    graph_->colors_.push_back({id, false, {}});
    graph_->passes_[pass_].colorCount++;
    return write(id, Access::ColorWrite);
}
RenderGraph::Builder& RenderGraph::Builder::color(ResourceId id, VkClearColorValue clear) {
    Attachment attachment;
    attachment.id = id;
    attachment.clear = true;
    attachment.value.color = clear;
    graph_->colors_.push_back(attachment);
    graph_->passes_[pass_].colorCount++;
    return write(id, Access::ColorWrite);
}
RenderGraph::Builder& RenderGraph::Builder::depth(ResourceId id, float clear) {
    graph_->passes_[pass_].depth = id;
    graph_->passes_[pass_].depthClear = clear;
    return write(id, Access::DepthWrite);
}
RenderGraph::Builder& RenderGraph::Builder::dispatch(VkPipeline pipeline, uint16_t divisor) {
    graph_->passes_[pass_].pipeline = pipeline;
    graph_->passes_[pass_].divisor = divisor;
    return *this;
}
RenderGraph::Builder& RenderGraph::Builder::record(std::function<void(const PassContext&)> body) {
    graph_->passes_[pass_].record = std::move(body);
    return *this;
}
RenderGraph::Builder& RenderGraph::Builder::sideEffect() {
    graph_->passes_[pass_].sideEffect = true;
    return *this;
}

// A pass survives if it writes something that outlives the frame or that a surviving pass
// reads. Conditional features therefore cost nothing when their consumer is absent.
void RenderGraph::cull() {
    needed_.assign(aliasRoot_.size() * 2, 0);
    auto& needed = needed_;
    live_ = 0;
    for (auto pass = passes_.rbegin(); pass != passes_.rend(); ++pass) {
        bool alive = pass->sideEffect;
        for (uint32_t i = 0; i < pass->count && !alive; ++i) {
            const auto& use = uses_[pass->first + i];
            if (!writes(use.access))
                continue;
            alive = pool_.declaration(use.ref.id).lifetime != Lifetime::Transient ||
                    needed[use.ref.id.index * 2 + (use.ref.slot == Slot::Previous)];
        }
        pass->alive = alive;
        if (!alive)
            continue;
        ++live_;
        for (uint32_t i = 0; i < pass->count; ++i) {
            const auto& use = uses_[pass->first + i];
            if (!writes(use.access) || has(use.access, Access::ComputeRead))
                needed[use.ref.id.index * 2 + (use.ref.slot == Slot::Previous)] = 1;
        }
    }
}

// Transients whose live ranges do not overlap can share one allocation. The ranges come
// from the surviving passes, so this is a property of the frame rather than a hand table.
void RenderGraph::alias(uint32_t width, uint32_t height) {
    struct Range {
        ResourceId id;
        uint32_t first = UINT32_MAX, last = 0;
    };
    std::vector<Range> ranges(aliasRoot_.size());
    for (size_t i = 0; i < ranges.size(); ++i)
        ranges[i].id = ResourceId{uint16_t(i)};
    for (uint32_t p = 0; p < passes_.size(); ++p) {
        if (!passes_[p].alive)
            continue;
        for (uint32_t i = 0; i < passes_[p].count; ++i) {
            auto& range = ranges[uses_[passes_[p].first + i].ref.id.index];
            range.first = std::min(range.first, p);
            range.last = std::max(range.last, p);
        }
    }
    for (size_t i = 0; i < aliasRoot_.size(); ++i)
        aliasRoot_[i] = ResourceId{uint16_t(i)};
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
        if (range.first != UINT32_MAX && pool_.declaration(range.id).lifetime == Lifetime::Transient)
            ordered.push_back(range);
    std::sort(ordered.begin(), ordered.end(),
              [](const Range& a, const Range& b) { return a.first < b.first; });
    for (const auto& range : ordered) {
        const uint64_t signature = storageSignature(pool_.declaration(range.id), width, height);
        auto tenant = std::find_if(tenants.begin(), tenants.end(), [&](const Tenant& t) {
            return t.signature == signature && t.last < range.first;
        });
        if (tenant == tenants.end())
            tenants.push_back({signature, range.last, range.id});
        else {
            aliasRoot_[range.id.index] = tenant->root;
            tenant->last = range.last;
            ++aliased_;
        }
    }
}

void RenderGraph::compile(uint32_t width, uint32_t height) {
    CpuScope scope("Graph / Compile");
    cull();
    alias(width, height);
    pool_.realize(width, height, aliasRoot_);
}

// One barrier batch per pass, derived from what every physical slot was last used for.
// The state lives in the pool and survives the frame boundary, so the first access of a
// ping-ponged history half is ordered against last frame's reads without any extra rule.
void RenderGraph::synchronise(VkCommandBuffer command, const Pass& pass) {
    merged_.clear();
    for (uint32_t i = 0; i < pass.count; ++i) {
        const auto& use = uses_[pass.first + i];
        if (pool_.declaration(use.ref.id).lifetime == Lifetime::External)
            continue;
        const uint32_t slot = pool_.physical(use.ref);
        auto found = std::find_if(merged_.begin(), merged_.end(), [&](const Use& other) {
            return pool_.physical(other.ref) == slot;
        });
        if (found == merged_.end())
            merged_.push_back(use);
        else
            found->access |= use.access;
    }
    imageBarriers_.clear();
    bufferBarriers_.clear();
    memoryBarriers_.clear();
    for (const auto& use : merged_) {
        const auto& declaration = pool_.declaration(use.ref.id);
        const auto info = accessInfo(use.access);
        auto& state = pool_.state(pool_.physical(use.ref));
        const bool isImage = declaration.kind == Kind::Image;
        Image* image = isImage ? &pool_.image(use.ref) : nullptr;
        const bool transition = isImage && image->layout != info.layout;
        const bool write = writes(use.access) || transition;
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
            const auto& buffer = pool_.buffer(use.ref);
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
            VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            barrier.srcStageMask = sourceStage ? sourceStage : VK_PIPELINE_STAGE_2_NONE;
            barrier.srcAccessMask = sourceAccess;
            barrier.dstStageMask = info.stage;
            barrier.dstAccessMask = info.access;
            memoryBarriers_.push_back(barrier);
        }
        if (write) {
            state.writeStage = info.stage;
            state.writeAccess = info.access;
            state.readStages = writes(use.access) ? 0 : info.stage;
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

void RenderGraph::execute(VkCommandBuffer command, GpuProfiler& profiler) {
    PassContext context;
    context.command = command;
    context.descriptors = pool_.descriptors();
    context.layout = pool_.pipelineLayout();
    context.pool = &pool_;
    barriers_ = 0;
    for (const auto& pass : passes_) {
        if (!pass.alive)
            continue;
        CpuScope cpuScope(pass.name);
        GpuScope gpuScope(profiler, command, pass.name);
        synchronise(command, pass);
        uint32_t width = pool_.width(), height = pool_.height();
        if (pass.colorCount || pass.depth.valid()) {
            const auto& reference = pass.colorCount ? colors_[pass.firstColor].id : pass.depth;
            width = pool_.extentWidth(reference);
            height = pool_.extentHeight(reference);
        }
        context.width = width;
        context.height = height;
        auto& attachments = attachments_;
        VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (pass.colorCount || pass.depth.valid()) {
            attachments.clear();
            for (uint32_t i = 0; i < pass.colorCount; ++i) {
                const auto& attachment = colors_[pass.firstColor + i];
                VkRenderingAttachmentInfo info{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                info.imageView = pool_.image(attachment.id).view;
                info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                info.loadOp = attachment.clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
                info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                info.clearValue = attachment.value;
                attachments.push_back(info);
            }
            VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
            rendering.renderArea = {{0, 0}, {width, height}};
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = uint32_t(attachments.size());
            rendering.pColorAttachments = attachments.data();
            if (pass.depth.valid()) {
                depthAttachment.imageView = pool_.image(pass.depth).view;
                depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                depthAttachment.clearValue.depthStencil = {pass.depthClear, 0};
                rendering.pDepthAttachment = &depthAttachment;
            }
            vkCmdBeginRendering(command, &rendering);
            VkViewport viewport{0, 0, float(width), float(height), 0, 1};
            VkRect2D scissor{{0, 0}, {width, height}};
            vkCmdSetViewport(command, 0, 1, &viewport);
            vkCmdSetScissor(command, 0, 1, &scissor);
        }
        if (pass.pipeline) {
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pass.pipeline);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, context.layout, 0, 1,
                                    &context.descriptors, 0, nullptr);
            const uint32_t w = (pool_.width() + pass.divisor - 1) / pass.divisor;
            const uint32_t h = (pool_.height() + pass.divisor - 1) / pass.divisor;
            vkCmdDispatch(command, (w + 7) / 8, (h + 7) / 8, 1);
        }
        if (pass.record)
            pass.record(context);
        if (pass.colorCount || pass.depth.valid())
            vkCmdEndRendering(command);
    }
}
} // namespace afterlight::rg
