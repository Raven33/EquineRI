// Device creation (P60, P92). D3D12 has no enable step, so the Vulkan device
// enables every *feature* the GPU reports, except those that cost performance
// just by being on (robustness) or that need something RI does not enable.
// Extensions are not all enabled: RI enables the ones it uses, when present;
// the user adds others through NativeDeviceAdditions.

#include "DeviceState.hpp"
#include "Instance.hpp"
#include "VkCheck.hpp"

#include <RI/Error.hpp>
#include <RI/Vk/Native.hpp>

#include <algorithm>
#include <cstring>
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
                    static_cast<void *>(&f.fault)}) {
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
    state->functions.vkGetDeviceQueue(state->device, state->graphicsQueueFamily, 0, &state->graphicsQueue);

    device.internal.state = state;
    return device;
}

void destroyDevice(Device &device) {
    DeviceState *state = device.internal.state;
    if (state == nullptr) {
        reportError(ErrorCategory::eProgrammerError, "destroyDevice: this device was never created or already destroyed");
    }
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
    };
}

} // namespace RI::Vk
