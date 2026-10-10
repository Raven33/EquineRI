#pragma once

// Native Vulkan access (P84). Including this header and naming the backend
// (`RI::Vk::native(device)`) is the only way to reach Vulkan objects.

#include <RI/Vk/Backend.hpp>

#include <volk.h>

#include <span>

namespace RI::Vk {

// How the Vulkan backend hides image layouts (L1, A', P78). Backend-internal:
// user code is identical in every mode (P16).
enum class LayoutMode : uint8 {
    eAuto,        // eUnified when VK_KHR_unified_image_layouts is on, else the measured choice (P78)
    eUnified,     // everything rests in GENERAL; VK_KHR_unified_image_layouts makes it optimal
    eGeneral,     // mode G: everything rests in GENERAL without the extension (no switches)
    eTransitions, // mode T: D3D12-style resting layouts, switched around RI's operations (P68)
};

struct NativeDevice {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    // Device-level functions for this device (P89). Core instance-level functions are volk's
    // globals; fetch instance extension functions with vkGetInstanceProcAddr(instance, ...).
    const VolkDeviceTable *functions = nullptr;
    uint32 graphicsQueueFamily = 0;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    uint32 apiVersion = 0; // the device's Vulkan version, at least 1.3
    LayoutMode layoutMode = LayoutMode::eAuto; // the mode in use (never eAuto)
};

NativeDevice native(const Device &device);

// Native handles of RI objects (P84).
inline VkDeviceMemory native(const Memory &memory) { return reinterpret_cast<VkDeviceMemory>(memory.internal.memory); }
inline VkBuffer native(const Buffer &buffer) { return reinterpret_cast<VkBuffer>(buffer.internal.buffer); }
inline VkCommandBuffer native(const CommandList &cmd) { return static_cast<VkCommandBuffer>(cmd.internal.commandBuffer); }

struct NativeDescriptorHeap {
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet resourceSet = VK_NULL_HANDLE; // set 0, binding 0: VK_DESCRIPTOR_TYPE_MUTABLE_EXT array
    VkDescriptorSet samplerSet = VK_NULL_HANDLE;  // set 1, binding 0: sampler array
};
inline NativeDescriptorHeap native(const DescriptorHeap &heap) {
    return {reinterpret_cast<VkDescriptorPool>(heap.internal.pool),
            reinterpret_cast<VkDescriptorSet>(heap.internal.resourceSet),
            reinterpret_cast<VkDescriptorSet>(heap.internal.samplerSet)};
}

// The native image and the layout it rests in (L1). Native code that leaves a
// texture in another layout must return it to this one (P27).
struct NativeTexture {
    VkImage image = VK_NULL_HANDLE;
    VkImageLayout restingLayout = VK_IMAGE_LAYOUT_UNDEFINED;
};
inline NativeTexture native(const Texture &texture) {
    return {reinterpret_cast<VkImage>(texture.internal.image), static_cast<VkImageLayout>(texture.internal.restingLayout)};
}
inline VkImageView native(const TextureView &view) { return reinterpret_cast<VkImageView>(view.internal.view); }
inline VkQueryPool native(const QueryPool &pool) { return reinterpret_cast<VkQueryPool>(pool.internal.pool); }

struct NativePipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
};
inline NativePipeline native(const Pipeline &pipeline) {
    return {reinterpret_cast<VkPipeline>(pipeline.internal.pipeline),
            reinterpret_cast<VkPipelineLayout>(pipeline.internal.layout)};
}

// The features RI enables (P60 (2), P92): every feature the GPU reports,
// minus robustness and capture/replay. Get them with defaultDeviceFeatures,
// change any field (e.g. turn robustness on), and pass them to createDevice.
// RI links the structures itself, so sType and pNext are ignored. Members of
// extensions RI does not enable on this GPU are ignored; v14 is used only on
// Vulkan 1.4 devices. After your changes RI still turns off features whose
// prerequisites are off (e.g. update-after-bind buffers when robust buffer
// access cannot support them), so the device stays valid.
struct DeviceFeatures {
    VkPhysicalDeviceFeatures core{};
    VkPhysicalDeviceVulkan11Features v11{};
    VkPhysicalDeviceVulkan12Features v12{};
    VkPhysicalDeviceVulkan13Features v13{};
    VkPhysicalDeviceVulkan14Features v14{};
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh{};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructure{};
    VkPhysicalDeviceRayQueryFeaturesKHR rayQuery{};
#ifdef VK_KHR_unified_image_layouts
    VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR unifiedLayouts{};
#endif
    VkPhysicalDeviceFaultFeaturesEXT fault{};
    VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mutableDescriptor{}; // descriptor heaps (P94)
};

DeviceFeatures defaultDeviceFeatures(const Gpu &gpu);

// Native additions at device creation (P60 (3)).
struct NativeDeviceAdditions {
    // Extra device extensions, enabled alongside RI's own.
    std::span<const char *const> extensions;
    // RI's features as changed by you (start from defaultDeviceFeatures).
    // nullptr: RI's default.
    const DeviceFeatures *features = nullptr;
    // Extra structures for VkDeviceCreateInfo, appended after RI's chain and
    // left untouched. Not for the structures in DeviceFeatures or their
    // single-feature equivalents (e.g. VkPhysicalDeviceImageRobustnessFeatures):
    // Vulkan forbids chaining both; change DeviceFeatures instead.
    const void *pNext = nullptr;
    // Forces a layout mode, for testing and measuring (P78). eUnified needs the extension.
    LayoutMode layoutMode = LayoutMode::eAuto;
};

Device createDevice(const DeviceDesc &desc, const NativeDeviceAdditions &additions);

} // namespace RI::Vk
