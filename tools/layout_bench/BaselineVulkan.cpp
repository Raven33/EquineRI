// The Vulkan baseline (P79): hand-written optimal per-texture layouts.
// Attachments are in attachment layouts while rendered, SHADER_READ_ONLY_OPTIMAL
// while sampled, GENERAL while written by compute; UNDEFINED is the old layout
// when contents are discarded; stage and access masks are exact on both sides.

#include "Bench.hpp"

#include <RI/Vk/Native.hpp>

#include <vector>

namespace bench {

namespace {

struct State {
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;
};

struct Target {
    VkImageLayout layout;
    VkPipelineStageFlags2 stages;
    VkAccessFlags2 access;
    bool write;
};

Target targetOf(Role role, PassKind kind) {
    switch (role) {
    case Role::eColorTarget:
        return {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT, true};
    case Role::eDepthTarget:
        return {VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT, true};
    case Role::eSampled:
        return {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                kind == PassKind::eCompute ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                                           : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false};
    case Role::eStorage:
        return {VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                true};
    }
    return {};
}

VkImageAspectFlags aspectOf(RI::Format format) {
    if (!RI::isDepthFormat(format)) {
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
    return RI::hasStencil(format) ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
}

} // namespace

struct Baseline {
    RI::Device *device = nullptr;
    RI::DescriptorHeap *heap = nullptr;
    RI::Vk::NativeDevice native;
    std::vector<State> states;
};

Baseline *createBaseline(RI::Device &device, RI::DescriptorHeap &heap) {
    return new Baseline{&device, &heap, RI::Vk::native(device), {}};
}
void destroyBaseline(Baseline *baseline) { delete baseline; }

// Vulkan descriptors carry the layout the image is in when read, so the
// baseline writes its own: SHADER_READ_ONLY_OPTIMAL for reads, GENERAL for storage.
void baselineWriteDescriptors(Baseline &b, Workload &w) {
    std::vector<VkDescriptorImageInfo> infos;
    std::vector<VkWriteDescriptorSet> writes;
    infos.reserve(w.textures.size() * 2);
    const VkDescriptorSet set = RI::Vk::native(*b.heap).resourceSet;
    for (Texture &t : w.textures) {
        if ((t.texture.usage & RI::TextureUsage::eSampled) != RI::TextureUsage::eNone) {
            infos.push_back({.imageView = RI::Vk::native(t.sampledView), .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
            writes.push_back({.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                              .dstSet = set,
                              .dstArrayElement = t.sampledSlot,
                              .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                              .pImageInfo = &infos.back()});
        }
        if ((t.texture.usage & RI::TextureUsage::eStorage) != RI::TextureUsage::eNone) {
            infos.push_back({.imageView = RI::Vk::native(t.texture.view), .imageLayout = VK_IMAGE_LAYOUT_GENERAL});
            writes.push_back({.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                              .dstSet = set,
                              .dstArrayElement = t.storageSlot,
                              .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                              .pImageInfo = &infos.back()});
        }
    }
    b.native.functions->vkUpdateDescriptorSets(b.native.device, static_cast<uint32>(writes.size()), writes.data(), 0,
                                               nullptr);
}

void baselineReset(Baseline &b, Workload &w) {
    b.states.assign(w.textures.size(), {});
    for (size_t i = 0; i < w.textures.size(); ++i) {
        b.states[i].layout = RI::Vk::native(w.textures[i].texture).restingLayout;
    }
}

void baselineBeforePass(Baseline &b, RI::CommandList &cmd, Workload &w, const Pass &pass) {
    std::vector<VkImageMemoryBarrier2> barriers;
    for (const Use &use : pass.uses) {
        State &s = b.states[use.texture];
        const RI::Format format = w.textures[use.texture].texture.format;
        Target t = targetOf(use.role, pass.kind);
        if (use.role == Role::eDepthTarget && RI::hasStencil(format)) {
            t.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        }
        constexpr VkAccessFlags2 kWrites = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                           VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        if (s.layout == t.layout && (s.access & kWrites) == 0 && !t.write) {
            s.stages |= t.stages; // read after read: no barrier
            continue;
        }
        const RI::Texture &tex = w.textures[use.texture].texture;
        barriers.push_back({
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = s.stages,
            .srcAccessMask = use.discard ? VK_ACCESS_2_NONE : (s.access & kWrites),
            .dstStageMask = t.stages,
            .dstAccessMask = t.access,
            .oldLayout = use.discard ? VK_IMAGE_LAYOUT_UNDEFINED : s.layout,
            .newLayout = t.layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = RI::Vk::native(tex).image,
            .subresourceRange = {.aspectMask = aspectOf(tex.format),
                                 .levelCount = VK_REMAINING_MIP_LEVELS,
                                 .layerCount = VK_REMAINING_ARRAY_LAYERS},
        });
        s = {t.layout, t.stages, t.access};
    }
    if (!barriers.empty()) {
        const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                          .imageMemoryBarrierCount = static_cast<uint32>(barriers.size()),
                                          .pImageMemoryBarriers = barriers.data()};
        b.native.functions->vkCmdPipelineBarrier2(RI::Vk::native(cmd), &dependency);
    }
}

void baselineBeginRendering(Baseline &b, RI::CommandList &cmd, Workload &w, const Pass &pass) {
    std::vector<VkRenderingAttachmentInfo> colors;
    VkRenderingAttachmentInfo depth{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo stencil{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    bool hasDepth = false;
    bool hasStencil = false;
    uint32 width = 0, height = 0;
    for (const Use &use : pass.uses) {
        const RI::TextureView &view = w.textures[use.texture].texture.view;
        const VkAttachmentLoadOp load = use.discard ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        if (use.role == Role::eColorTarget) {
            colors.push_back({.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                              .imageView = RI::Vk::native(view),
                              .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              .loadOp = load,
                              .storeOp = VK_ATTACHMENT_STORE_OP_STORE});
        } else if (use.role == Role::eDepthTarget) {
            depth.imageView = RI::Vk::native(view);
            depth.imageLayout = RI::hasStencil(view.format) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                                            : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depth.loadOp = load;
            depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depth.clearValue.depthStencil = {1.0f, 0};
            hasDepth = true;
            if (RI::hasStencil(view.format)) {
                stencil = depth;
                hasStencil = true;
            }
        } else {
            continue;
        }
        width = view.width;
        height = view.height;
    }
    const VkRenderingInfo info{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                               .renderArea = {{0, 0}, {width, height}},
                               .layerCount = 1,
                               .colorAttachmentCount = static_cast<uint32>(colors.size()),
                               .pColorAttachments = colors.data(),
                               .pDepthAttachment = hasDepth ? &depth : nullptr,
                               .pStencilAttachment = hasStencil ? &stencil : nullptr};
    const VolkDeviceTable &fn = *b.native.functions;
    const VkCommandBuffer commandBuffer = RI::Vk::native(cmd);
    fn.vkCmdBeginRendering(commandBuffer, &info);
    // The same viewport as RI (y up, as on D3D12).
    const VkViewport viewport{0, static_cast<float>(height), static_cast<float>(width), -static_cast<float>(height), 0, 1};
    const VkRect2D scissor{{0, 0}, {width, height}};
    fn.vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    fn.vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
}

void baselineEndRendering(Baseline &b, RI::CommandList &cmd) {
    b.native.functions->vkCmdEndRendering(RI::Vk::native(cmd));
}

} // namespace bench
