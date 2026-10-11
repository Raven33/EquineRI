#include "Instance.hpp"

#include "VkCheck.hpp"

#include <RI/Error.hpp>
#include <RI/Vk/Backend.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>
#include <string_view>
#include <vector>

namespace RI::Vk {

namespace {

constexpr const char *kValidationLayer = "VK_LAYER_KHRONOS_validation";

// Enabled when the loader offers them; the swapchain (P43) needs them.
constexpr const char *kSurfaceExtensions[] = {
    "VK_KHR_surface",       "VK_KHR_win32_surface",   "VK_KHR_xlib_surface",
    "VK_KHR_xcb_surface",   "VK_KHR_wayland_surface", "VK_EXT_swapchain_colorspace", // HDR10, scRGB (P64)
};

void loadLoader() {
    static bool loaded = false;
    if (loaded) {
        return;
    }
    checkVk(volkInitialize(), "volkInitialize (no Vulkan loader found)");
    const uint32 version = volkGetInstanceVersion();
    if (version < kMinApiVersion) {
        reportError(ErrorCategory::eUnsupported,
                    std::format("the Vulkan loader is version {}.{}, RI needs 1.3 or newer",
                                VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version)));
    }
    loaded = true;
}

bool hasExtension(const std::vector<VkExtensionProperties> &available, const char *name) {
    return std::ranges::any_of(available, [&](const VkExtensionProperties &e) {
        return std::strcmp(e.extensionName, name) == 0;
    });
}

// Validation messages (P21): validation-layer errors go to the error channel
// as programmer errors. Everything else is printed, including the loader's own
// errors (GENERAL type, e.g. a stale driver manifest), which are not the
// program's fault.
VKAPI_ATTR VkBool32 VKAPI_CALL onValidationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                   VkDebugUtilsMessageTypeFlagsEXT types,
                                                   const VkDebugUtilsMessengerCallbackDataEXT *data,
                                                   void * /*user*/) {
    const char *message = data != nullptr && data->pMessage != nullptr ? data->pMessage : "(no message)";
    const bool error = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
    if (error && (types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0) {
        reportError(ErrorCategory::eProgrammerError, std::format("Vulkan validation: {}", message));
    }
    std::fprintf(stderr, "[EquineRI] Vulkan %s: %s\n", error ? "error" : "warning", message);
    return VK_FALSE;
}

} // namespace

InstanceHandles createInstance(InstancePurpose purpose) {
    loadLoader();

    std::vector<const char *> layers;
    std::vector<const char *> extensions;
    VkDebugUtilsMessengerCreateInfoEXT messengerInfo{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = &onValidationMessage,
    };
    const bool validation = EQUINE_VALIDATION && purpose == InstancePurpose::eDevice;
    // Validation builds (P53 (3), (4)): shader printf (DXC printf, printed to
    // stdout) and GPU-assisted validation (bindless indices, buffer addresses).
    const VkBool32 on = VK_TRUE;
    const VkLayerSettingEXT settings[] = {
        {kValidationLayer, "printf_enable", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &on},
        {kValidationLayer, "printf_to_stdout", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &on},
        {kValidationLayer, "gpuav_enable", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &on},
    };
    const VkLayerSettingsCreateInfoEXT layerSettings{.sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT,
                                                     .settingCount = static_cast<uint32>(std::size(settings)),
                                                     .pSettings = settings};
    messengerInfo.pNext = &layerSettings;

    if (purpose == InstancePurpose::eDevice) {
        uint32 count = 0;
        checkVk(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr), "vkEnumerateInstanceExtensionProperties");
        std::vector<VkExtensionProperties> available(count);
        checkVk(vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data()),
                "vkEnumerateInstanceExtensionProperties");

        // Object names and command markers are always on (P53).
        if (!hasExtension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            reportError(ErrorCategory::eUnsupported, "the Vulkan loader lacks VK_EXT_debug_utils");
        }
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        for (const char *name : kSurfaceExtensions) {
            if (hasExtension(available, name)) {
                extensions.push_back(name);
            }
        }

        if (validation) {
            checkVk(vkEnumerateInstanceLayerProperties(&count, nullptr), "vkEnumerateInstanceLayerProperties");
            std::vector<VkLayerProperties> layerList(count);
            checkVk(vkEnumerateInstanceLayerProperties(&count, layerList.data()), "vkEnumerateInstanceLayerProperties");
            const bool found = std::ranges::any_of(layerList, [](const VkLayerProperties &l) {
                return std::strcmp(l.layerName, kValidationLayer) == 0;
            });
            if (!found) {
                reportError(ErrorCategory::eUnsupported,
                            "this is a validation build (EQUINE_VALIDATION) but VK_LAYER_KHRONOS_validation is not "
                            "installed; install the Vulkan SDK or your distribution's validation-layers package");
            }
            layers.push_back(kValidationLayer);
        }
    }

    const VkApplicationInfo appInfo{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pEngineName = "EquineRI",
        .apiVersion = VK_API_VERSION_1_4, // the highest version RI uses; devices need only 1.3
    };
    const VkInstanceCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = validation ? &messengerInfo : nullptr, // also catches vkCreate/DestroyInstance messages
        .pApplicationInfo = &appInfo,
        .enabledLayerCount = static_cast<uint32>(layers.size()),
        .ppEnabledLayerNames = layers.data(),
        .enabledExtensionCount = static_cast<uint32>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };

    InstanceHandles handles;
    checkVk(vkCreateInstance(&createInfo, nullptr, &handles.instance), "vkCreateInstance");
    // volk's instance-level globals now come from the newest instance. RI calls
    // only core instance functions through them: those are the loader's
    // trampolines, which dispatch on the handle passed in, so they stay valid
    // for every instance. Extension functions are NULL when loaded from an
    // instance without the extension (the listing instance), so RI fetches
    // those per instance and keeps them in InstanceHandles.
    volkLoadInstanceOnly(handles.instance);

    if (validation) {
        const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(handles.instance, "vkCreateDebugUtilsMessengerEXT"));
        handles.destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(handles.instance, "vkDestroyDebugUtilsMessengerEXT"));
        checkVk(create(handles.instance, &messengerInfo, nullptr, &handles.messenger), "vkCreateDebugUtilsMessengerEXT");
    }
    return handles;
}

void destroyInstance(InstanceHandles &handles) {
    if (handles.messenger != VK_NULL_HANDLE) {
        handles.destroyMessenger(handles.instance, handles.messenger, nullptr);
    }
    if (handles.instance != VK_NULL_HANDLE) {
        vkDestroyInstance(handles.instance, nullptr);
    }
    handles = {};
}

bool describeGpu(VkPhysicalDevice physicalDevice, Gpu &gpu, uint32 &graphicsQueueFamily) {
    VkPhysicalDeviceIDProperties idProperties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                           .pNext = &idProperties};
    vkGetPhysicalDeviceProperties2(physicalDevice, &properties);
    const VkPhysicalDeviceProperties &p = properties.properties;
    if (p.apiVersion < kMinApiVersion) {
        return false;
    }

    uint32 familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, families.data());
    constexpr VkQueueFlags kGraphicsCompute = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    const auto family = std::ranges::find_if(families, [](const VkQueueFamilyProperties &f) {
        return (f.queueFlags & kGraphicsCompute) == kGraphicsCompute && f.queueCount > 0;
    });
    if (family == families.end()) {
        return false;
    }
    graphicsQueueFamily = static_cast<uint32>(family - families.begin());

    gpu.name = p.deviceName;
    gpu.vendorId = p.vendorID;
    gpu.deviceId = p.deviceID;
    switch (p.vendorID) {
    case 0x10DE: gpu.vendor = GpuVendor::eNvidia; break;
    case 0x1002:
    case 0x1022: gpu.vendor = GpuVendor::eAMD; break;
    case 0x8086: gpu.vendor = GpuVendor::eIntel; break;
    default: gpu.vendor = GpuVendor::eOther; break;
    }
    switch (p.deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: gpu.kind = GpuKind::eDiscrete; break;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: gpu.kind = GpuKind::eIntegrated; break;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: gpu.kind = GpuKind::eSoftware; break;
    default: gpu.kind = GpuKind::eOther; break;
    }

    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memory);
    gpu.memory = 0;
    for (uint32 i = 0; i < memory.memoryHeapCount; ++i) {
        if ((memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
            gpu.memory = std::max<uint64>(gpu.memory, memory.memoryHeaps[i].size);
        }
    }

    static_assert(sizeof(gpu.id) == VK_UUID_SIZE);
    std::memcpy(gpu.id.data(), idProperties.deviceUUID, VK_UUID_SIZE);
    return true;
}

VkPhysicalDevice findGpu(VkInstance instance, const Gpu &gpu, Gpu &found, uint32 &graphicsQueueFamily) {
    uint32 count = 0;
    checkVk(vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
    std::vector<VkPhysicalDevice> physicalDevices(count);
    checkVk(vkEnumeratePhysicalDevices(instance, &count, physicalDevices.data()), "vkEnumeratePhysicalDevices");
    for (VkPhysicalDevice physicalDevice : physicalDevices) {
        Gpu candidate;
        if (describeGpu(physicalDevice, candidate, graphicsQueueFamily) && candidate.id == gpu.id) {
            found = std::move(candidate);
            return physicalDevice;
        }
    }
    reportError(ErrorCategory::eProgrammerError,
                std::format("the GPU '{}' is not available; pass a GPU returned by listGpus()", gpu.name));
}

std::vector<Gpu> listGpus() {
    InstanceHandles handles = createInstance(InstancePurpose::eListing);
    uint32 count = 0;
    checkVk(vkEnumeratePhysicalDevices(handles.instance, &count, nullptr), "vkEnumeratePhysicalDevices");
    std::vector<VkPhysicalDevice> physicalDevices(count);
    checkVk(vkEnumeratePhysicalDevices(handles.instance, &count, physicalDevices.data()), "vkEnumeratePhysicalDevices");

    std::vector<Gpu> gpus;
    for (VkPhysicalDevice physicalDevice : physicalDevices) {
        Gpu gpu;
        uint32 family = 0;
        if (describeGpu(physicalDevice, gpu, family)) {
            gpus.push_back(std::move(gpu));
        }
    }
    destroyInstance(handles);
    return gpus;
}

} // namespace RI::Vk
