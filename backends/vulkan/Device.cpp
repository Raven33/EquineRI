// Device creation on Vulkan (P60, P89, P92).
//
// Owns: createDevice, destroyDevice, defaultDeviceFeatures, and the choices
// made once per device: which extensions RI enables (chooseSetup), which
// features are turned on (every feature the GPU reports, except robustness
// and capture/replay), the memory type of each kind (chooseMemoryTypes), the
// descriptor set layouts behind heaps (createHeapLayouts), the timeline and
// the internal command buffer.
//
// Watch out: DGC is left off on NVIDIA under GPU-AV (the driver faults, P98
// (5)). Ray tracing is turned off when the driver accepts no mutable heap
// with acceleration structures, and the heap size is lowered to what the
// driver accepts (P98 (8)).
//
// D3D12 counterpart: backends/d3d12/Device.cpp.

#include "DeviceState.hpp"
#include "Instance.hpp"
#include "VkCheck.hpp"

#include <RI/Error.hpp>
#include <RI/Vk/Native.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <vector>

namespace RI::Vk {

namespace {

bool hasExtension(const std::vector<VkExtensionProperties> &available, const char *name) {
    return std::ranges::any_of(available, [&](const VkExtensionProperties &e) {
        return std::strcmp(e.extensionName, name) == 0;
    });
}

// What RI enables on one GPU: its extensions, and which feature structures apply.
struct DeviceSetup {
    std::vector<const char *> extensions;
    bool v14 = false;
    bool mesh = false;
    bool accelerationStructure = false; // with deferred host operations
    bool rayQuery = false;
    bool unifiedLayouts = false;
    bool fault = false;
    bool mutableDescriptor = false;
    bool generatedCommands = false;
};

// RI's extensions (P60 (2)), each enabled when the GPU has it.
DeviceSetup chooseSetup(VkPhysicalDevice physicalDevice, uint32 apiVersion) {
    uint32 count = 0;
    checkVk(vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr),
            "vkEnumerateDeviceExtensionProperties");
    std::vector<VkExtensionProperties> available(count);
    checkVk(vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, available.data()),
            "vkEnumerateDeviceExtensionProperties");

    DeviceSetup setup;
    setup.v14 = apiVersion >= VK_API_VERSION_1_4;
    const auto use = [&](const char *name) {
        if (hasExtension(available, name)) {
            setup.extensions.push_back(name);
            return true;
        }
        return false;
    };
    setup.mutableDescriptor = use(VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME); // descriptor heaps (P94), required
    use(VK_KHR_SWAPCHAIN_EXTENSION_NAME);           // P43
    setup.mesh = use(VK_EXT_MESH_SHADER_EXTENSION_NAME); // P39
    // P49: acceleration structures need deferred host operations; ray query needs both.
    if (hasExtension(available, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) &&
        hasExtension(available, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)) {
        use(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        setup.accelerationStructure = use(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        setup.rayQuery = use(VK_KHR_RAY_QUERY_EXTENSION_NAME);
    }
#ifdef VK_KHR_unified_image_layouts
    setup.unifiedLayouts = use(VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME); // L1, P78
#endif
    if (EQUINE_VALIDATION) {
        setup.fault = use(VK_EXT_DEVICE_FAULT_EXTENSION_NAME); // P53 (2)
    }
    // Indirect Level 2 (P48); it relies on maintenance5, core in Vulkan 1.4.
    // Not on NVIDIA in validation builds with GPU-assisted validation: GPU-AV's
    // instrumentation of generated commands made the RTX 3080 (driver 617.42)
    // fault and reset (2026-10-11); EQUINE_GPU_VALIDATION=0 brings it back.
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &properties);
    const bool gpuAvOnNvidia = EQUINE_VALIDATION && gpuValidationEnabled() && properties.vendorID == 0x10DE;
    if (setup.v14 && !gpuAvOnNvidia) {
        setup.generatedCommands = use(VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME);
    }
    return setup;
}

// RI's features as a linked chain for one GPU. Built from a DeviceFeatures:
// sType and pNext are always set here, whatever the user's copy holds.
struct FeatureChain {
    VkPhysicalDeviceFeatures2 head{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    DeviceFeatures f;

    // Links the structures that apply to `setup`; returns the head.
    VkPhysicalDeviceFeatures2 *link(const DeviceSetup &setup, const void *tail) {
        f.v11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        f.v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        f.v13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        f.v14.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
        f.mesh.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
        f.accelerationStructure.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        f.rayQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
#ifdef VK_KHR_unified_image_layouts
        f.unifiedLayouts.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR;
#endif
        f.fault.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
        f.mutableDescriptor.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT;
        f.generatedCommands.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT;

        std::vector<void *> chain{&f.v11, &f.v12, &f.v13};
        if (setup.v14) {
            chain.push_back(&f.v14);
        }
        if (setup.mesh) {
            chain.push_back(&f.mesh);
        }
        if (setup.accelerationStructure) {
            chain.push_back(&f.accelerationStructure);
        }
        if (setup.rayQuery) {
            chain.push_back(&f.rayQuery);
        }
#ifdef VK_KHR_unified_image_layouts
        if (setup.unifiedLayouts) {
            chain.push_back(&f.unifiedLayouts);
        }
#endif
        if (setup.fault) {
            chain.push_back(&f.fault);
        }
        if (setup.mutableDescriptor) {
            chain.push_back(&f.mutableDescriptor);
        }
        if (setup.generatedCommands) {
            chain.push_back(&f.generatedCommands);
        }

        head.features = f.core;
        head.pNext = chain.front();
        for (size_t i = 0; i < chain.size(); ++i) {
            static_cast<VkBaseOutStructure *>(chain[i])->pNext =
                i + 1 < chain.size() ? static_cast<VkBaseOutStructure *>(chain[i + 1])
                                     : static_cast<VkBaseOutStructure *>(const_cast<void *>(tail));
        }
        return &head;
    }
};

// Everything the GPU supports, minus robustness and capture/replay (P60 (2)).
DeviceFeatures queryDefaultFeatures(VkPhysicalDevice physicalDevice, const DeviceSetup &setup) {
    FeatureChain chain;
    vkGetPhysicalDeviceFeatures2(physicalDevice, chain.link(setup, nullptr));
    DeviceFeatures f = chain.f;
    f.core = chain.head.features;
    // The links point into `chain`; DeviceFeatures carries none (RI relinks).
    for (void *s : {static_cast<void *>(&f.v11), static_cast<void *>(&f.v12), static_cast<void *>(&f.v13),
                    static_cast<void *>(&f.v14), static_cast<void *>(&f.mesh), static_cast<void *>(&f.accelerationStructure),
                    static_cast<void *>(&f.rayQuery),
#ifdef VK_KHR_unified_image_layouts
                    static_cast<void *>(&f.unifiedLayouts),
#endif
                    static_cast<void *>(&f.fault), static_cast<void *>(&f.mutableDescriptor),
                    static_cast<void *>(&f.generatedCommands)}) {
        static_cast<VkBaseOutStructure *>(s)->pNext = nullptr;
    }
    // Robustness costs performance just by being on. Turn it on through
    // DeviceFeatures (defaultDeviceFeatures + createDevice additions).
    f.core.robustBufferAccess = VK_FALSE;
    f.v13.robustImageAccess = VK_FALSE;
    f.v14.pipelineRobustness = VK_FALSE;
    // Capture/replay support is for capture tools and can change how drivers
    // place allocations.
    f.v12.bufferDeviceAddressCaptureReplay = VK_FALSE;
    f.accelerationStructure.accelerationStructureCaptureReplay = VK_FALSE;
    return f;
}

// Turns off features whose prerequisites are off, after the user's changes,
// so vkCreateDevice stays valid whatever DeviceFeatures holds.
void resolveDependencies(VkPhysicalDevice physicalDevice, DeviceFeatures &f) {
    VkPhysicalDeviceVulkan12Properties v12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &v12};
    vkGetPhysicalDeviceProperties2(physicalDevice, &properties);
    // VUID-VkDeviceCreateInfo-robustBufferAccess-10247
    if (f.core.robustBufferAccess == VK_TRUE && v12.robustBufferAccessUpdateAfterBind == VK_FALSE) {
        f.v12.descriptorBindingUniformBufferUpdateAfterBind = VK_FALSE;
        f.v12.descriptorBindingStorageBufferUpdateAfterBind = VK_FALSE;
        f.v12.descriptorBindingUniformTexelBufferUpdateAfterBind = VK_FALSE;
        f.v12.descriptorBindingStorageTexelBufferUpdateAfterBind = VK_FALSE;
    }
    if (f.v11.multiview == VK_FALSE) {
        f.mesh.multiviewMeshShader = VK_FALSE;
    }
    // Needs VK_KHR_fragment_shading_rate, which RI does not enable (F2).
    f.mesh.primitiveFragmentShadingRateMeshShader = VK_FALSE;
    if (f.v12.bufferDeviceAddress == VK_FALSE) {
        f.accelerationStructure.accelerationStructure = VK_FALSE;
    }
    if (f.accelerationStructure.accelerationStructure == VK_FALSE) {
        f.rayQuery.rayQuery = VK_FALSE;
    }
#ifdef VK_KHR_unified_image_layouts
    // Needs the video extensions, which RI does not enable.
    f.unifiedLayouts.unifiedImageLayoutsVideo = VK_FALSE;
#endif
}

// Features RI itself relies on. Missing ones (unsupported, or turned off in
// DeviceFeatures) make device creation fail with a readable error.
void requireFeatures(const DeviceSetup &setup, const DeviceFeatures &f) {
    std::string missing;
    const auto need = [&](VkBool32 value, const char *name) {
        if (value != VK_TRUE) {
            missing += missing.empty() ? name : std::string(", ") + name;
        }
    };
    need(setup.mutableDescriptor ? VK_TRUE : VK_FALSE, "VK_EXT_mutable_descriptor_type");
    need(f.mutableDescriptor.mutableDescriptorType, "mutableDescriptorType");
    need(f.v12.timelineSemaphore, "timelineSemaphore");
    need(f.v12.bufferDeviceAddress, "bufferDeviceAddress");
    need(f.v12.descriptorIndexing, "descriptorIndexing");
    need(f.v12.runtimeDescriptorArray, "runtimeDescriptorArray");
    need(f.v12.descriptorBindingPartiallyBound, "descriptorBindingPartiallyBound");
    need(f.v12.descriptorBindingVariableDescriptorCount, "descriptorBindingVariableDescriptorCount");
    need(f.v12.descriptorBindingStorageBufferUpdateAfterBind, "descriptorBindingStorageBufferUpdateAfterBind");
    need(f.v12.descriptorBindingUniformBufferUpdateAfterBind, "descriptorBindingUniformBufferUpdateAfterBind");
    need(f.v12.descriptorBindingSampledImageUpdateAfterBind, "descriptorBindingSampledImageUpdateAfterBind");
    need(f.v12.descriptorBindingStorageImageUpdateAfterBind, "descriptorBindingStorageImageUpdateAfterBind");
    need(f.v12.descriptorBindingUniformTexelBufferUpdateAfterBind, "descriptorBindingUniformTexelBufferUpdateAfterBind");
    need(f.v12.descriptorBindingStorageTexelBufferUpdateAfterBind, "descriptorBindingStorageTexelBufferUpdateAfterBind");
    need(f.v13.synchronization2, "synchronization2");
    need(f.v13.dynamicRendering, "dynamicRendering");                     // P40
    need(f.v12.separateDepthStencilLayouts, "separateDepthStencilLayouts"); // depth-only views (P73)
    need(f.v12.hostQueryReset, "hostQueryReset");                           // P71 (1)
    need(f.core.occlusionQueryPrecise, "occlusionQueryPrecise");            // P71: exact sample counts
    need(f.core.pipelineStatisticsQuery, "pipelineStatisticsQuery");        // P71 (2)
    need(f.v12.drawIndirectCount, "drawIndirectCount");                     // P48
    need(f.core.multiDrawIndirect, "multiDrawIndirect");                     // P48
    need(f.core.fillModeNonSolid, "fillModeNonSolid");                       // FillMode::eWireframe (P102)
    need(f.core.depthClamp, "depthClamp");                                   // P102
    need(f.core.depthBiasClamp, "depthBiasClamp");                           // P102
    need(f.core.imageCubeArray, "imageCubeArray");                           // cube arrays (P102)
    if (!missing.empty()) {
        reportError(ErrorCategory::eUnsupported,
                    std::format("this GPU or your DeviceFeatures lack what RI needs: {}", missing));
    }
}

// Memory kinds (P32) to native memory types. Types must accept buffers (any
// memory kind holds any buffer); AMD's device-coherent/uncached types, protected
// and lazily allocated types are never used.
void chooseMemoryTypes(DeviceState &state, DeviceLimits &limits) {
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(state.physicalDevice, &memory);

    const VkBufferCreateInfo probeInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 65536,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    };
    const VkDeviceBufferMemoryRequirements probe{.sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS,
                                                 .pCreateInfo = &probeInfo};
    VkMemoryRequirements2 requirements{.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    state.functions.vkGetDeviceBufferMemoryRequirements(state.device, &probe, &requirements);
    const uint32 bufferTypes = requirements.memoryRequirements.memoryTypeBits;

    constexpr VkMemoryPropertyFlags kNever = VK_MEMORY_PROPERTY_PROTECTED_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT |
                                             VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD |
                                             VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD;
    // The first type with all `required` flags, none of `avoid` if possible.
    const auto pick = [&](VkMemoryPropertyFlags required, VkMemoryPropertyFlags avoid, VkMemoryPropertyFlags prefer) {
        uint32 best = UINT32_MAX;
        int bestScore = -1;
        for (uint32 i = 0; i < memory.memoryTypeCount; ++i) {
            const VkMemoryPropertyFlags flags = memory.memoryTypes[i].propertyFlags;
            if ((bufferTypes & (1u << i)) == 0 || (flags & required) != required || (flags & kNever) != 0) {
                continue;
            }
            const int score = ((flags & avoid) == 0 ? 2 : 0) + ((flags & prefer) == prefer ? 1 : 0);
            if (score > bestScore) {
                best = i;
                bestScore = score;
            }
        }
        return best;
    };
    constexpr VkMemoryPropertyFlags kLocal = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    constexpr VkMemoryPropertyFlags kHost = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    state.memoryTypes[kindIndex(MemoryKind::eGPU)] = pick(kLocal, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0);
    state.memoryTypes[kindIndex(MemoryKind::eUpload)] = pick(kHost, kLocal | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);
    state.memoryTypes[kindIndex(MemoryKind::eReadback)] =
        pick(kHost, kLocal, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);

    // GPU memory the CPU can write (P32): only when all of the GPU's memory is
    // CPU-visible (ReBAR), as on D3D12; a small BAR window does not count.
    const uint32 gpuUpload = pick(kLocal | kHost, 0, 0);
    VkDeviceSize largestLocalHeap = 0;
    for (uint32 i = 0; i < memory.memoryHeapCount; ++i) {
        if ((memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
            largestLocalHeap = std::max(largestLocalHeap, memory.memoryHeaps[i].size);
        }
    }
    if (gpuUpload != UINT32_MAX && memory.memoryHeaps[memory.memoryTypes[gpuUpload].heapIndex].size == largestLocalHeap) {
        state.memoryTypes[kindIndex(MemoryKind::eGPUUpload)] = gpuUpload;
        limits.gpuUploadMemory = true;
    }

    // Other types with the kind's flags, for resources the default type does
    // not fit (allocateMemory with `forResource`).
    const VkMemoryPropertyFlags required[4] = {kLocal, kHost, kHost, kLocal | kHost};
    for (uint32 kind = 0; kind < 4; ++kind) {
        const uint32 first = state.memoryTypes[kind];
        if (first == UINT32_MAX) {
            continue;
        }
        state.memoryTypeCandidates[kind].push_back(first);
        for (uint32 i = 0; i < memory.memoryTypeCount; ++i) {
            const VkMemoryPropertyFlags flags = memory.memoryTypes[i].propertyFlags;
            // Same heap kind as the default: GPU memory stays CPU-invisible
            // where the default is, CPU memory stays off the GPU heap.
            const VkMemoryPropertyFlags sameAsDefault =
                memory.memoryTypes[first].propertyFlags & (kLocal | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
            if (i != first && (flags & required[kind]) == required[kind] && (flags & kNever) == 0 &&
                (flags & (kLocal | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) == sameAsDefault) {
                state.memoryTypeCandidates[kind].push_back(i);
            }
        }
    }
}

// The device-wide descriptor set layouts behind descriptor heaps (P94): set 0
// holds resource views as mutable descriptors (DXC puts every
// ResourceDescriptorHeap access at set 0, binding 0); set 1 holds samplers
// (shaders are compiled with -fvk-bind-sampler-heap 0 1). Both bindings have a
// variable count, so one layout fits heaps of every size.
void createHeapLayouts(DeviceState &state, DeviceLimits &limits) {
    VkPhysicalDeviceVulkan12Properties v12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &v12};
    vkGetPhysicalDeviceProperties2(state.physicalDevice, &properties);
    const VkPhysicalDeviceLimits &l = properties.properties.limits;
    state.storageOffsetAlignment = l.minStorageBufferOffsetAlignment;
    state.bufferImageGranularity = l.bufferImageGranularity;
    state.uniformOffsetAlignment = l.minUniformBufferOffsetAlignment;

    // D3D12's resource binding tier 3 limits, or less if the GPU has less.
    limits.maxResourceDescriptors = std::min({1'000'000u, v12.maxDescriptorSetUpdateAfterBindStorageBuffers,
                                              v12.maxDescriptorSetUpdateAfterBindUniformBuffers,
                                              v12.maxDescriptorSetUpdateAfterBindSampledImages,
                                              v12.maxDescriptorSetUpdateAfterBindStorageImages,
                                              v12.maxPerStageDescriptorUpdateAfterBindStorageBuffers,
                                              v12.maxPerStageDescriptorUpdateAfterBindUniformBuffers,
                                              v12.maxPerStageDescriptorUpdateAfterBindSampledImages,
                                              v12.maxPerStageDescriptorUpdateAfterBindStorageImages,
                                              v12.maxPerStageUpdateAfterBindResources});
    limits.maxSamplerDescriptors = std::min({2048u, v12.maxDescriptorSetUpdateAfterBindSamplers,
                                             v12.maxPerStageDescriptorUpdateAfterBindSamplers});
    limits.maxRootDataSize = l.maxPushConstantsSize;
    limits.maxBufferViewSize = l.maxStorageBufferRange;
    limits.maxConstantViewSize = std::min<uint32>(l.maxUniformBufferRange, 65536); // D3D12's limit (P106)

    state.mutableTypes = {
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    };
    if (state.rayTracing) {
        state.mutableTypes.push_back(VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR); // P49
    }
    VkMutableDescriptorTypeListEXT typeList{.descriptorTypeCount = static_cast<uint32>(state.mutableTypes.size()),
                                            .pDescriptorTypes = state.mutableTypes.data()};
    const VkMutableDescriptorTypeCreateInfoEXT mutableInfo{
        .sType = VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT,
        .mutableDescriptorTypeListCount = 1,
        .pMutableDescriptorTypeLists = &typeList,
    };
    constexpr VkDescriptorBindingFlags kBindingFlags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                                       VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                                       VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
    const auto create = [&](VkDescriptorType type, uint32 count, const void *next, const char *what) {
        const VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
            .pNext = next,
            .bindingCount = 1,
            .pBindingFlags = &kBindingFlags,
        };
        const VkDescriptorSetLayoutBinding binding{
            .binding = 0, .descriptorType = type, .descriptorCount = count, .stageFlags = VK_SHADER_STAGE_ALL};
        const VkDescriptorSetLayoutCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = &flagsInfo,
            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
            .bindingCount = 1,
            .pBindings = &binding,
        };
        VkDescriptorSetLayoutSupport support{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT};
        state.functions.vkGetDescriptorSetLayoutSupport(state.device, &info, &support);
        if (support.supported != VK_TRUE) {
            reportError(ErrorCategory::eUnsupported,
                        std::format("the GPU cannot hold a {} descriptor heap of {} slots", what, count));
        }
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        checkVk(state.functions.vkCreateDescriptorSetLayout(state.device, &info, nullptr, &layout),
                "vkCreateDescriptorSetLayout");
        return layout;
    };
    // The largest heap the driver accepts: the property limits are an upper
    // bound, but drivers may refuse less for mutable descriptors.
    const auto supported = [&](uint32 count) {
        const VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
            .pNext = &mutableInfo,
            .bindingCount = 1,
            .pBindingFlags = &kBindingFlags,
        };
        const VkDescriptorSetLayoutBinding binding{.binding = 0,
                                                   .descriptorType = VK_DESCRIPTOR_TYPE_MUTABLE_EXT,
                                                   .descriptorCount = count,
                                                   .stageFlags = VK_SHADER_STAGE_ALL};
        const VkDescriptorSetLayoutCreateInfo info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                   .pNext = &flagsInfo,
                                                   .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
                                                   .bindingCount = 1,
                                                   .pBindings = &binding};
        VkDescriptorSetLayoutSupport support{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT};
        state.functions.vkGetDescriptorSetLayoutSupport(state.device, &info, &support);
        return support.supported == VK_TRUE;
    };
    // DXC puts acceleration structures at the same binding as every other
    // heap access, so they must be a mutable type. Some drivers (AMD Windows)
    // accept no mutable heap with them: ray tracing is then off (P49, P11).
    if (state.rayTracing && !supported(1)) {
        state.mutableTypes.pop_back();
        typeList.descriptorTypeCount = static_cast<uint32>(state.mutableTypes.size());
        state.rayTracing = false;
        limits.rayTracing = false;
        limits.accelerationStructureScratchAlignment = 0;
    }
    if (!supported(1)) {
        reportError(ErrorCategory::eUnsupported, "the GPU supports no mutable descriptor heap of RI's types");
    }
    if (!supported(limits.maxResourceDescriptors)) {
        uint32 low = 1;
        uint32 high = limits.maxResourceDescriptors;
        while (high - low > 1024) {
            const uint32 middle = low + (high - low) / 2;
            (supported(middle) ? low : high) = middle;
        }
        limits.maxResourceDescriptors = low;
    }
    state.resourceSetLayout =
        create(VK_DESCRIPTOR_TYPE_MUTABLE_EXT, limits.maxResourceDescriptors, &mutableInfo, "resource");
    state.samplerSetLayout = create(VK_DESCRIPTOR_TYPE_SAMPLER, limits.maxSamplerDescriptors, nullptr, "sampler");
}

// The layout mode (L1, P78, P95): eUnified when the extension's feature is
// on; otherwise mode T until the measurement decides (P78). A forced eUnified
// without the feature is unsupported.
LayoutMode chooseLayoutMode(const DeviceSetup &setup, const DeviceFeatures &f, LayoutMode requested) {
    bool unified = false;
#ifdef VK_KHR_unified_image_layouts
    unified = setup.unifiedLayouts && f.unifiedLayouts.unifiedImageLayouts == VK_TRUE;
#else
    (void)setup;
    (void)f;
#endif
    if (requested == LayoutMode::eUnified && !unified) {
        reportError(ErrorCategory::eUnsupported,
                    "LayoutMode::eUnified needs VK_KHR_unified_image_layouts and its unifiedImageLayouts feature, "
                    "which this GPU or your DeviceFeatures lack");
    }
    if (requested != LayoutMode::eAuto) {
        return requested;
    }
    return unified ? LayoutMode::eUnified : LayoutMode::eTransitions;
}

// Timestamp ticks per second (P71 (3)). Differences of timestamps need no
// masking by the queue's timestampValidBits; absolute values may wrap.
void readTimestampFrequency(DeviceState &state, DeviceLimits &limits) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(state.physicalDevice, &properties);
    limits.timestampFrequency = static_cast<uint64>(std::llround(1e9 / double(properties.limits.timestampPeriod)));
}

// RI's own command pool and buffer, for texture resets at creation (R16).
void createInternalCommands(DeviceState &state) {
    const VkCommandPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                           .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                           .queueFamilyIndex = state.graphicsQueueFamily};
    checkVk(state.functions.vkCreateCommandPool(state.device, &poolInfo, nullptr, &state.internalPool),
            "vkCreateCommandPool");
    const VkCommandBufferAllocateInfo allocInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                .commandPool = state.internalPool,
                                                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                .commandBufferCount = 1};
    checkVk(state.functions.vkAllocateCommandBuffers(state.device, &allocInfo, &state.internalCommands),
            "vkAllocateCommandBuffers");
}

void createTimeline(DeviceState &state) {
    const VkSemaphoreTypeCreateInfo typeInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    const VkSemaphoreCreateInfo info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &typeInfo};
    checkVk(state.functions.vkCreateSemaphore(state.device, &info, nullptr, &state.timeline), "vkCreateSemaphore");
}

uint32 apiVersionOf(VkPhysicalDevice physicalDevice) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &properties);
    return properties.apiVersion;
}

} // namespace

DeviceFeatures defaultDeviceFeatures(const Gpu &gpu) {
    InstanceHandles instance = createInstance(InstancePurpose::eListing);
    Gpu found;
    uint32 family = 0;
    const VkPhysicalDevice physicalDevice = findGpu(instance.instance, gpu, found, family);
    DeviceFeatures features = queryDefaultFeatures(physicalDevice, chooseSetup(physicalDevice, apiVersionOf(physicalDevice)));
    destroyInstance(instance);
    return features;
}

Device createDevice(const DeviceDesc &desc) { return createDevice(desc, {}); }

Device createDevice(const DeviceDesc &desc, const NativeDeviceAdditions &additions) {
    auto *state = new DeviceState{};
#if EQUINE_VALIDATION
    state->deviceThread = std::this_thread::get_id();
#endif
    const InstanceHandles instance = createInstance(InstancePurpose::eDevice);
    state->instance = instance.instance;
    state->messenger = instance.messenger;
    state->destroyMessenger = instance.destroyMessenger;

    Device device;
    state->physicalDevice = findGpu(state->instance, desc.gpu, device.gpu, state->graphicsQueueFamily);
    state->apiVersion = apiVersionOf(state->physicalDevice);

    DeviceSetup setup = chooseSetup(state->physicalDevice, state->apiVersion);
    for (const char *name : additions.extensions) {
        if (std::ranges::none_of(setup.extensions, [&](const char *e) { return std::strcmp(e, name) == 0; })) {
            setup.extensions.push_back(name);
        }
    }

    FeatureChain chain;
    chain.f = additions.features != nullptr ? *additions.features : queryDefaultFeatures(state->physicalDevice, setup);
    resolveDependencies(state->physicalDevice, chain.f);
    requireFeatures(setup, chain.f);
    state->layoutMode = chooseLayoutMode(setup, chain.f, additions.layoutMode);
    state->hostBarrierAtEnd = additions.hostBarrierAtEnd;

    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queueInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = state->graphicsQueueFamily,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const VkDeviceCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = chain.link(setup, additions.pNext),
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queueInfo,
        .enabledExtensionCount = static_cast<uint32>(setup.extensions.size()),
        .ppEnabledExtensionNames = setup.extensions.data(),
    };
    checkVk(vkCreateDevice(state->physicalDevice, &createInfo, nullptr, &state->device), "vkCreateDevice");

    // Device-level functions go through this device's own table (P89).
    volkLoadDeviceTable(&state->functions, state->device);
    loadDebugFunctions(*state);
    {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(state->physicalDevice, &properties);
        state->copyOffsetAlignment = std::max<VkDeviceSize>(properties.limits.optimalBufferCopyOffsetAlignment, 1);
        state->copyRowPitchAlignment = std::max<VkDeviceSize>(properties.limits.optimalBufferCopyRowPitchAlignment, 1);
    }
    state->faultEnabled = setup.fault && chain.f.fault.deviceFault == VK_TRUE;
    device.limits.meshShaders = setup.mesh && chain.f.mesh.meshShader == VK_TRUE && chain.f.mesh.taskShader == VK_TRUE;
    device.limits.geometryShaders = chain.f.core.geometryShader == VK_TRUE;
    device.limits.tessellation = chain.f.core.tessellationShader == VK_TRUE;
    // 64-bit atomics (P105): Vulkan 1.2 features, on when the GPU has them (P60).
    const bool int64 = chain.f.core.shaderInt64 == VK_TRUE;
    device.limits.int64BufferAtomics = int64 && chain.f.v12.shaderBufferInt64Atomics == VK_TRUE;
    device.limits.int64SharedAtomics = int64 && chain.f.v12.shaderSharedInt64Atomics == VK_TRUE;
    state->pipelineStatistics =
        VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT | VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
        VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT |
        VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT | VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT |
        VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;
    if (device.limits.geometryShaders) {
        state->pipelineStatistics |= VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_INVOCATIONS_BIT |
                                     VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT;
    }
    if (device.limits.tessellation) {
        state->pipelineStatistics |= VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_CONTROL_SHADER_PATCHES_BIT |
                                     VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_EVALUATION_SHADER_INVOCATIONS_BIT;
    }
    device.limits.indirectRootData =
        setup.generatedCommands && chain.f.generatedCommands.deviceGeneratedCommands == VK_TRUE;
    state->generatedCommands = device.limits.indirectRootData;
    // Inline ray tracing (P49): acceleration structures and ray queries.
    state->rayTracing = setup.rayQuery && chain.f.accelerationStructure.accelerationStructure == VK_TRUE &&
                        chain.f.rayQuery.rayQuery == VK_TRUE;
    device.limits.rayTracing = state->rayTracing;
    {
        // Samplers (P87): min/max reduction, and the driver's sampler limits.
        state->samplerMinMax = chain.f.v12.samplerFilterMinmax == VK_TRUE;
        device.limits.samplerMinMax = state->samplerMinMax;
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(state->physicalDevice, &properties);
        state->maxSamplers = properties.limits.maxSamplerAllocationCount;
        state->maxAnisotropy = chain.f.core.samplerAnisotropy == VK_TRUE ? properties.limits.maxSamplerAnisotropy : 1.0f;
    }
    if (state->rayTracing) {
        VkPhysicalDeviceAccelerationStructurePropertiesKHR asProperties{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 properties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                               .pNext = &asProperties};
        vkGetPhysicalDeviceProperties2(state->physicalDevice, &properties);
        state->scratchAlignment = std::max<VkDeviceSize>(asProperties.minAccelerationStructureScratchOffsetAlignment, 1);
        device.limits.accelerationStructureScratchAlignment = static_cast<uint32>(state->scratchAlignment);
    }
    {
        // Depth and stencil resolve modes (P76).
        VkPhysicalDeviceDepthStencilResolveProperties resolve{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                               .pNext = &resolve};
        vkGetPhysicalDeviceProperties2(state->physicalDevice, &properties);
        const auto modes = [](VkResolveModeFlags flags) {
            uint32 bits = 0;
            bits |= (flags & VK_RESOLVE_MODE_AVERAGE_BIT) != 0 ? resolveModeBit(ResolveMode::eAverage) : 0;
            bits |= (flags & VK_RESOLVE_MODE_MIN_BIT) != 0 ? resolveModeBit(ResolveMode::eMin) : 0;
            bits |= (flags & VK_RESOLVE_MODE_MAX_BIT) != 0 ? resolveModeBit(ResolveMode::eMax) : 0;
            bits |= (flags & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT) != 0 ? resolveModeBit(ResolveMode::eSampleZero) : 0;
            return bits;
        };
        device.limits.depthResolveModes = modes(resolve.supportedDepthResolveModes);
        // One mode for both aspects (P76): without independent resolve, Vulkan
        // needs them equal, which RI's single mode guarantees.
        device.limits.stencilResolveModes = modes(resolve.supportedStencilResolveModes);
        state->depthResolveModes = device.limits.depthResolveModes;
        state->stencilResolveModes = device.limits.stencilResolveModes;
    }
    state->functions.vkGetDeviceQueue(state->device, state->graphicsQueueFamily, 0, &state->graphicsQueue);

    chooseMemoryTypes(*state, device.limits);
    createHeapLayouts(*state, device.limits);
    createTimeline(*state);
    createInternalCommands(*state);
    readTimestampFrequency(*state, device.limits);

    device.graphicsQueue.internal.device = state;
    device.internal.state = state;
    return device;
}

void destroyDevice(Device &device) {
    EQ_VK_CHECK_THREAD(device.internal.state);
    DeviceState *state = device.internal.state;
    if (state == nullptr) {
        reportError(ErrorCategory::eProgrammerError, "destroyDevice: this device was never created or already destroyed");
    }
    destroyIndirectLayouts(*state);
    for (const auto &[key, sampler] : state->samplers) {
        state->functions.vkDestroySampler(state->device, sampler, nullptr);
    }
    state->functions.vkDestroyCommandPool(state->device, state->internalPool, nullptr);
    state->functions.vkDestroySemaphore(state->device, state->timeline, nullptr);
    state->functions.vkDestroyDescriptorSetLayout(state->device, state->resourceSetLayout, nullptr);
    state->functions.vkDestroyDescriptorSetLayout(state->device, state->samplerSetLayout, nullptr);
    state->functions.vkDestroyDevice(state->device, nullptr);
    InstanceHandles instance{
        .instance = state->instance, .messenger = state->messenger, .destroyMessenger = state->destroyMessenger};
    destroyInstance(instance);
    delete state;
    device.internal.state = nullptr;
}

NativeDevice native(const Device &device) {
    const DeviceState &state = *device.internal.state;
    return {
        .instance = state.instance,
        .physicalDevice = state.physicalDevice,
        .device = state.device,
        .functions = &state.functions,
        .graphicsQueueFamily = state.graphicsQueueFamily,
        .graphicsQueue = state.graphicsQueue,
        .apiVersion = state.apiVersion,
        .layoutMode = state.layoutMode,
    };
}

} // namespace RI::Vk
