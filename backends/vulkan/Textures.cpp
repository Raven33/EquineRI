// Textures, views and their layouts (P58, P69, P73, P74, P78, P95). Layouts
// never reach the user (P16): each texture rests in one layout chosen from its
// usage and the device's layout mode, and RI switches it only around its own
// operations (A', P68).

#include "DeviceState.hpp"
#include "VkCheck.hpp"

#include <RI/Error.hpp>
#include <RI/Validation/Checks.hpp>
#include <RI/Vk/Native.hpp>

#include <algorithm>
#include <format>
#include <vector>

namespace RI::Vk {

namespace {

struct FormatInfo {
    Format format;
    VkFormat native;
    const char *name;
};

// Every RI format, in enum order.
constexpr FormatInfo kFormats[] = {
    {Format::eUndefined, VK_FORMAT_UNDEFINED, "eUndefined"},
    {Format::eR8Unorm, VK_FORMAT_R8_UNORM, "eR8Unorm"},
    {Format::eRG8Unorm, VK_FORMAT_R8G8_UNORM, "eRG8Unorm"},
    {Format::eRGBA8Unorm, VK_FORMAT_R8G8B8A8_UNORM, "eRGBA8Unorm"},
    {Format::eRGBA8Srgb, VK_FORMAT_R8G8B8A8_SRGB, "eRGBA8Srgb"},
    {Format::eBGRA8Unorm, VK_FORMAT_B8G8R8A8_UNORM, "eBGRA8Unorm"},
    {Format::eBGRA8Srgb, VK_FORMAT_B8G8R8A8_SRGB, "eBGRA8Srgb"},
    {Format::eRGB10A2Unorm, VK_FORMAT_A2B10G10R10_UNORM_PACK32, "eRGB10A2Unorm"},
    {Format::eR11G11B10Float, VK_FORMAT_B10G11R11_UFLOAT_PACK32, "eR11G11B10Float"},
    {Format::eR16Float, VK_FORMAT_R16_SFLOAT, "eR16Float"},
    {Format::eRG16Float, VK_FORMAT_R16G16_SFLOAT, "eRG16Float"},
    {Format::eRGBA16Float, VK_FORMAT_R16G16B16A16_SFLOAT, "eRGBA16Float"},
    {Format::eR32Float, VK_FORMAT_R32_SFLOAT, "eR32Float"},
    {Format::eRG32Float, VK_FORMAT_R32G32_SFLOAT, "eRG32Float"},
    {Format::eRGBA32Float, VK_FORMAT_R32G32B32A32_SFLOAT, "eRGBA32Float"},
    {Format::eR32Uint, VK_FORMAT_R32_UINT, "eR32Uint"},
    {Format::eRG32Uint, VK_FORMAT_R32G32_UINT, "eRG32Uint"},
    {Format::eRGBA32Uint, VK_FORMAT_R32G32B32A32_UINT, "eRGBA32Uint"},
    {Format::eD16Unorm, VK_FORMAT_D16_UNORM, "eD16Unorm"},
    {Format::eD32Float, VK_FORMAT_D32_SFLOAT, "eD32Float"},
    {Format::eD24UnormS8Uint, VK_FORMAT_D24_UNORM_S8_UINT, "eD24UnormS8Uint"},
    {Format::eD32FloatS8Uint, VK_FORMAT_D32_SFLOAT_S8_UINT, "eD32FloatS8Uint"},
};

const FormatInfo &infoOf(Format format) {
    const auto index = static_cast<size_t>(format);
    if (index >= std::size(kFormats)) {
        reportError(ErrorCategory::eProgrammerError, std::format("unknown texture format {}", index));
    }
    return kFormats[index];
}

static_assert(std::size(kFormats) <= 32, "Texture::internal.viewFormats is a 32-bit mask");
uint32 formatBit(Format format) { return 1u << static_cast<uint32>(format); }

bool has(TextureUsage usage, TextureUsage bit) { return (usage & bit) != TextureUsage::eNone; }

// Native usage. Render targets, depth and storage textures can also be
// cleared (P65), which Vulkan does with transfer commands.
VkImageUsageFlags usageOf(TextureUsage usage) {
    VkImageUsageFlags flags = 0;
    if (has(usage, TextureUsage::eSampled)) {
        flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (has(usage, TextureUsage::eStorage)) {
        flags |= VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    if (has(usage, TextureUsage::eRenderTarget)) {
        flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    if (has(usage, TextureUsage::eDepthStencil)) {
        flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    if (has(usage, TextureUsage::eCopySrc)) {
        flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if (has(usage, TextureUsage::eCopyDst)) {
        flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    return flags;
}

// The VkImageCreateInfo of a texture, with its format list (P74). Filled in
// place because the structures point at each other.
struct ImageInfo {
    VkImageCreateInfo info{};
    VkImageFormatListCreateInfo list{};
    std::vector<VkFormat> formats;
};

void fillImageInfo(const TextureDesc &desc, ImageInfo &out) {
    out.info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = toVkFormat(desc.format),
        .extent = {desc.width, desc.height, 1},
        .mipLevels = desc.mips,
        .arrayLayers = desc.layers,
        .samples = static_cast<VkSampleCountFlagBits>(desc.samples),
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usageOf(desc.usage),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (desc.viewFormats.empty()) {
        return;
    }
    // An exact list lets drivers keep compression (P74).
    out.formats.push_back(out.info.format);
    for (Format format : desc.viewFormats) {
        const VkFormat native = toVkFormat(format);
        if (std::ranges::find(out.formats, native) == out.formats.end()) {
            out.formats.push_back(native);
        }
    }
    out.list = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
                .viewFormatCount = static_cast<uint32>(out.formats.size()),
                .pViewFormats = out.formats.data()};
    out.info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    out.info.pNext = &out.list;
}

VkFormatFeatureFlags2 featuresOf(VkPhysicalDevice physicalDevice, VkFormat format) {
    VkFormatProperties3 properties3{.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
    VkFormatProperties2 properties{.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &properties3};
    vkGetPhysicalDeviceFormatProperties2(physicalDevice, format, &properties);
    return properties3.optimalTilingFeatures;
}

VkImageAspectFlags aspectsOf(Format format, Aspect aspect) {
    switch (aspect) {
    case Aspect::eDefault:
        if (isDepthFormat(format)) {
            return VK_IMAGE_ASPECT_DEPTH_BIT | (hasStencil(format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        }
        return VK_IMAGE_ASPECT_COLOR_BIT;
    case Aspect::eColor:
        return isDepthFormat(format) ? 0 : VK_IMAGE_ASPECT_COLOR_BIT;
    case Aspect::eDepth:
        return isDepthFormat(format) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0;
    case Aspect::eStencil:
        return hasStencil(format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0;
    }
    return 0;
}

} // namespace

// ── Formats and layouts ──────────────────────────────────────────────────────

VkFormat toVkFormat(Format format) { return infoOf(format).native; }
const char *formatName(Format format) { return infoOf(format).name; }

// P95 (2): one resting layout per texture. Without unified layouts, mode T
// picks the optimal layout of the texture's main use, as D3D12 would. Sampled
// depth rests in SHADER_READ_ONLY_OPTIMAL (fast sampling, P97) unless it is
// sampled during read-only depth sections, which need the depth read-only
// layout (READ_ONLY_OPTIMAL, P75).
VkImageLayout restingLayoutOf(LayoutMode mode, Format format, TextureUsage usage) {
    if (mode != LayoutMode::eTransitions || has(usage, TextureUsage::eStorage)) {
        return VK_IMAGE_LAYOUT_GENERAL;
    }
    if (has(usage, TextureUsage::eSampled)) {
        if (isDepthFormat(format) && !has(usage, TextureUsage::eReadOnlyDepthSampling)) {
            return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        return VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL; // color reads, and depth reads during read-only sections (sync2)
    }
    if (has(usage, TextureUsage::eRenderTarget)) {
        return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    if (has(usage, TextureUsage::eDepthStencil)) {
        return VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
    }
    return VK_IMAGE_LAYOUT_GENERAL;
}

VkImageSubresourceRange rangeOf(const TextureView &view) {
    return {.aspectMask = view.internal.aspectMask,
            .baseMipLevel = view.internal.baseMip,
            .levelCount = view.internal.mipCount,
            .baseArrayLayer = view.internal.baseLayer,
            .layerCount = view.internal.layerCount};
}

VkImageMemoryBarrier2 imageBarrier(const TextureView &view, const ImageTransition &t) {
    return {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = t.srcStage,
        .srcAccessMask = t.srcAccess,
        .dstStageMask = t.dstStage,
        .dstAccessMask = t.dstAccess,
        .oldLayout = t.oldLayout,
        .newLayout = t.newLayout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = reinterpret_cast<VkImage>(view.internal.image),
        .subresourceRange = rangeOf(view),
    };
}

// ── RI's own submissions (R16) ───────────────────────────────────────────────

VkCommandBuffer beginInternal(DeviceState &state) {
    checkVk(state.functions.vkResetCommandPool(state.device, state.internalPool, 0), "vkResetCommandPool");
    const VkCommandBufferBeginInfo info{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    checkVk(state.functions.vkBeginCommandBuffer(state.internalCommands, &info), "vkBeginCommandBuffer");
    return state.internalCommands;
}

void submitInternal(DeviceState &state) {
    checkVk(state.functions.vkEndCommandBuffer(state.internalCommands), "vkEndCommandBuffer");
    const uint64 value = state.lastSignaled + 1;
    const VkCommandBufferSubmitInfo commandInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                                .commandBuffer = state.internalCommands};
    const VkSemaphoreSubmitInfo signal{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                       .semaphore = state.timeline,
                                       .value = value,
                                       .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    const VkSubmitInfo2 submitInfo{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                                   .commandBufferInfoCount = 1,
                                   .pCommandBufferInfos = &commandInfo,
                                   .signalSemaphoreInfoCount = 1,
                                   .pSignalSemaphoreInfos = &signal};
    checkVk(state.functions.vkQueueSubmit2(state.graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE), "vkQueueSubmit2");
    state.lastSignaled = value;
    const VkSemaphoreWaitInfo waitInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                       .semaphoreCount = 1,
                                       .pSemaphores = &state.timeline,
                                       .pValues = &value};
    checkVk(state.functions.vkWaitSemaphores(state.device, &waitInfo, UINT64_MAX), "vkWaitSemaphores");
}

// ── Textures ─────────────────────────────────────────────────────────────────

bool formatSupported(Device &device, Format format, TextureUsage usage, uint32 samples) {
    const DeviceState &state = *device.internal.state;
    if (format == Format::eUndefined || static_cast<size_t>(format) >= std::size(kFormats)) {
        return false;
    }
    const VkFormatFeatureFlags2 features = featuresOf(state.physicalDevice, toVkFormat(format));
    const auto needs = [&](TextureUsage bit, VkFormatFeatureFlags2 feature) {
        return !has(usage, bit) || (features & feature) == feature;
    };
    if (!needs(TextureUsage::eSampled, VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT) ||
        !needs(TextureUsage::eStorage, VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT) ||
        !needs(TextureUsage::eRenderTarget, VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT) ||
        !needs(TextureUsage::eDepthStencil, VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT) ||
        !needs(TextureUsage::eCopySrc, VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT) ||
        !needs(TextureUsage::eCopyDst | TextureUsage::eStorage | TextureUsage::eRenderTarget |
                   TextureUsage::eDepthStencil,
               VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT)) {
        return false;
    }
    // The exact image RI would create, including the sample count.
    ImageInfo image;
    fillImageInfo({.samples = samples, .format = format, .usage = usage}, image);
    const VkPhysicalDeviceImageFormatInfo2 info{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
                                                .format = image.info.format,
                                                .type = image.info.imageType,
                                                .tiling = image.info.tiling,
                                                .usage = image.info.usage,
                                                .flags = image.info.flags};
    VkImageFormatProperties2 properties{.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    if (vkGetPhysicalDeviceImageFormatProperties2(state.physicalDevice, &info, &properties) != VK_SUCCESS) {
        return false;
    }
    return (properties.imageFormatProperties.sampleCounts & samples) != 0;
}

MemoryRequirements textureRequirements(Device &device, const TextureDesc &desc) {
    DeviceState &state = *device.internal.state;
    ImageInfo image;
    fillImageInfo(desc, image);
    const VkDeviceImageMemoryRequirements info{.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,
                                               .pCreateInfo = &image.info};
    VkMemoryRequirements2 requirements{.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    state.functions.vkGetDeviceImageMemoryRequirements(state.device, &info, &requirements);
    return withGranularity(state, {.size = requirements.memoryRequirements.size,
                                   .alignment = requirements.memoryRequirements.alignment,
                                   .kinds = kindsOf(state, requirements.memoryRequirements.memoryTypeBits)});
}

Texture createTexture(Device &device, const Memory &memory, uint64 offset, const TextureDesc &desc) {
    DeviceState &state = *device.internal.state;
    EQ_VK_CHECK_ALIVE(&state, memory.internal.id, "memory");
    EQ_VALIDATE(Validation::checkTextureUsage(desc.format, desc.usage));
    ImageInfo image;
    fillImageInfo(desc, image);
    VkImage vkImage = VK_NULL_HANDLE;
    checkVk(state.functions.vkCreateImage(state.device, &image.info, nullptr, &vkImage), "vkCreateImage");

    VkMemoryRequirements requirements{};
    state.functions.vkGetImageMemoryRequirements(state.device, vkImage, &requirements);
    if ((requirements.memoryTypeBits & (1u << memory.internal.memoryType)) == 0) {
        state.functions.vkDestroyImage(state.device, vkImage, nullptr);
        reportError(ErrorCategory::eUnsupported,
                    std::format("createTexture: this {} texture cannot be placed in this memory kind on '{}' "
                                "(check textureRequirements(...).kinds)",
                                formatName(desc.format), device.gpu.name));
    }
    EQ_VALIDATE(Validation::checkRange(offset, requirements.size, memory.size, requirements.alignment,
                                       "createTexture: placing the texture in memory"));
    checkVk(state.functions.vkBindImageMemory(state.device, vkImage, native(memory), offset), "vkBindImageMemory");

    Texture texture;
    texture.width = desc.width;
    texture.height = desc.height;
    texture.layers = desc.layers;
    texture.mips = desc.mips;
    texture.samples = desc.samples;
    texture.format = desc.format;
    texture.usage = desc.usage;
    texture.internal.image = reinterpret_cast<uint64>(vkImage);
    texture.internal.restingLayout = static_cast<uint32>(restingLayoutOf(state.layoutMode, desc.format, desc.usage));
    for (Format format : desc.viewFormats) {
        texture.internal.viewFormats |= formatBit(format);
    }
    texture.internal.id = EQ_VK_TRACK(&state);
#if EQUINE_VALIDATION
    Validation::placeResource(state.placements, memory.internal.id, offset, requirements.size, texture.internal.id,
                              true);
#endif
    texture.view = createView(device, texture, {});

    // The initial reset (R16, P69): contents undefined, now in the resting layout.
    const VkImageMemoryBarrier2 barrier =
        imageBarrier(texture.view, {.newLayout = static_cast<VkImageLayout>(texture.internal.restingLayout)});
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
    state.functions.vkCmdPipelineBarrier2(beginInternal(state), &dependency);
    submitInternal(state);
    return texture;
}

void destroyTexture(Device &device, Texture &texture) {
    DeviceState &state = *device.internal.state;
    EQ_VK_CHECK_ALIVE(&state, texture.internal.id, "texture");
    EQ_VK_UNTRACK(&state, texture.internal.id);
#if EQUINE_VALIDATION
    Validation::markDiscarded(state.placements, texture.internal.id);
#endif
    state.functions.vkDestroyImageView(state.device, native(texture.view), nullptr);
    state.functions.vkDestroyImage(state.device, native(texture).image, nullptr);
    texture = {};
}

// ── Views (P73, P74) ─────────────────────────────────────────────────────────

TextureView createView(Device &device, const Texture &texture, const TextureViewDesc &desc) {
    DeviceState &state = *device.internal.state;
    EQ_VK_CHECK_ALIVE(&state, texture.internal.id, "texture");
    const Format format = desc.format == Format::eUndefined ? texture.format : desc.format;
    // In every build: an unlisted format is invalid on every driver (P74).
    if (format != texture.format && (texture.internal.viewFormats & formatBit(format)) == 0) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("createView: format {} is not the texture's format or one of its viewFormats (P74)",
                                formatName(format)));
    }
    EQ_VALIDATE(Validation::checkViewFormatCompatible(texture.format, format));
    const VkImageAspectFlags aspects = aspectsOf(texture.format, desc.aspect);
    if (aspects == 0) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("createView: the texture's format {} has no such aspect", formatName(texture.format)));
    }
    const uint32 mipCount = desc.mipCount == kAllMips ? texture.mips - std::min(desc.baseMip, texture.mips) : desc.mipCount;
    const uint32 layerCount =
        desc.layerCount == kAllLayers ? texture.layers - std::min(desc.baseLayer, texture.layers) : desc.layerCount;
    EQ_VALIDATE(Validation::checkViewRange(desc.baseMip, mipCount, texture.mips, desc.baseLayer, layerCount,
                                           texture.layers));

    // A reinterpreting view keeps only the usages its own format supports
    // (e.g. no storage for sRGB).
    VkImageViewUsageCreateInfo usageInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                         .usage = usageOf(texture.usage)};
    if (format != texture.format) {
        const VkFormatFeatureFlags2 features = featuresOf(state.physicalDevice, toVkFormat(format));
        if ((features & VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT) == 0) {
            usageInfo.usage &= ~VkImageUsageFlags{VK_IMAGE_USAGE_STORAGE_BIT};
        }
        if ((features & VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT) == 0) {
            usageInfo.usage &= ~VkImageUsageFlags{VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
        }
        if ((features & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT) == 0) {
            usageInfo.usage &= ~VkImageUsageFlags{VK_IMAGE_USAGE_SAMPLED_BIT};
        }
    }
    const VkImageViewCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = format != texture.format ? &usageInfo : nullptr,
        .image = native(texture).image,
        // Array or not follows the texture, as on D3D12 and in HLSL (Texture2DArray).
        .viewType = texture.layers == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY,
        .format = toVkFormat(format),
        .subresourceRange = {.aspectMask = aspects,
                             .baseMipLevel = desc.baseMip,
                             .levelCount = mipCount,
                             .baseArrayLayer = desc.baseLayer,
                             .layerCount = layerCount},
    };
    VkImageView view = VK_NULL_HANDLE;
    checkVk(state.functions.vkCreateImageView(state.device, &info, nullptr, &view), "vkCreateImageView");

    TextureView out;
    out.format = format;
    out.width = std::max(1u, texture.width >> desc.baseMip);
    out.height = std::max(1u, texture.height >> desc.baseMip);
    out.samples = texture.samples;
    out.internal.view = reinterpret_cast<uint64>(view);
    out.internal.image = texture.internal.image;
    out.internal.aspectMask = aspects;
    out.internal.baseMip = desc.baseMip;
    out.internal.mipCount = mipCount;
    out.internal.baseLayer = desc.baseLayer;
    out.internal.layerCount = layerCount;
    out.internal.restingLayout = texture.internal.restingLayout;
    out.internal.textureUsage = texture.usage;
    out.internal.textureId = texture.internal.id;
    return out;
}

void destroyView(Device &device, TextureView &view) {
    DeviceState &state = *device.internal.state;
    EQ_VK_CHECK_ALIVE(&state, view.internal.textureId, "texture");
    state.functions.vkDestroyImageView(state.device, native(view), nullptr);
    view = {};
}

} // namespace RI::Vk
