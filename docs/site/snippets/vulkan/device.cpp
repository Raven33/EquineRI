// Canonical Vulkan: create an instance, list the GPUs, create a device with
// every supported feature except robustness, destroy everything.
//
// A stand-alone program: it uses only Vulkan and links the Vulkan loader
// directly (no RI). It is not what RI's backend does line by line; it is how
// you would do the same thing in your own Vulkan renderer.
//
// The docs build compiles it against RI's Vulkan headers, which define
// VK_NO_PROTOTYPES for volk, so the prototypes are turned back on here.
#undef VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

int main() {
    // --8<-- [start:create-instance]
    const VkApplicationInfo appInfo{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "my toy renderer",
        .apiVersion = VK_API_VERSION_1_3, // the highest version this program uses
    };
#ifndef NDEBUG
    // Debug builds: the validation layer (installed with the Vulkan SDK).
    const char *const layers[] = {"VK_LAYER_KHRONOS_validation"};
    const uint32_t layerCount = 1;
#else
    const char *const *layers = nullptr;
    const uint32_t layerCount = 0;
#endif
    const VkInstanceCreateInfo instanceInfo{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo,
        .enabledLayerCount = layerCount,
        .ppEnabledLayerNames = layers,
    };
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS) {
        return 1;
    }
    // --8<-- [end:create-instance]

    // --8<-- [start:enumerate-gpus]
    // The usual two-call pattern: ask for the count, then for the handles.
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> physicalDevices(count);
    vkEnumeratePhysicalDevices(instance, &count, physicalDevices.data());

    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    for (VkPhysicalDevice physicalDevice : physicalDevices) {
        // --8<-- [start:describe-gpu]
        // Name, vendor, type and version, plus a stable ID through the pNext chain.
        VkPhysicalDeviceIDProperties id{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &id};
        vkGetPhysicalDeviceProperties2(physicalDevice, &properties);
        const VkPhysicalDeviceProperties &p = properties.properties;
        if (p.apiVersion < VK_API_VERSION_1_3) {
            continue; // too old for this program
        }
        // p.vendorID is the PCI vendor ID (0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel).
        // p.deviceType tells discrete, integrated, CPU (software) apart.
        // id.deviceUUID identifies this GPU across instances and processes.

        // Memory: the largest heap that lives on the GPU.
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memory);
        VkDeviceSize vram = 0;
        for (uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
            if ((memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
                vram = std::max(vram, memory.memoryHeaps[i].size);
            }
        }
        std::printf("%s (%llu MiB)\n", p.deviceName, static_cast<unsigned long long>(vram >> 20));
        // --8<-- [end:describe-gpu]

        // A queue family that can do both graphics and compute.
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, families.data());
        for (uint32_t i = 0; i < familyCount; ++i) {
            const VkQueueFlags needed = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if (chosen == VK_NULL_HANDLE && (families[i].queueFlags & needed) == needed) {
                chosen = physicalDevice;
                queueFamily = i;
            }
        }
    }
    // --8<-- [end:enumerate-gpus]
    if (chosen == VK_NULL_HANDLE) {
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    // --8<-- [start:create-device]
    // 1. Ask which features the GPU supports, with a chain of feature structures.
    //    Each structure points to the next through pNext.
    VkPhysicalDeviceVulkan13Features v13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features v12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &v13};
    VkPhysicalDeviceVulkan11Features v11{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &v12};
    VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v11};
    vkGetPhysicalDeviceFeatures2(chosen, &features);

    // 2. The same chain now holds VK_TRUE for every supported feature. Turn off
    //    what you do not want: robustness costs performance just by being on.
    features.features.robustBufferAccess = VK_FALSE;
    v13.robustImageAccess = VK_FALSE;
    v12.bufferDeviceAddressCaptureReplay = VK_FALSE; // for capture tools only

    // 3. Create the device with one queue, passing the chain in pNext.
    //    pEnabledFeatures stays null: the features travel in the chain instead.
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queueInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queueFamily,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const VkDeviceCreateInfo deviceInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &features,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queueInfo,
        // Device extensions (e.g. VK_KHR_swapchain) go in ppEnabledExtensionNames,
        // after checking vkEnumerateDeviceExtensionProperties.
    };
    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(chosen, &deviceInfo, nullptr, &device) != VK_SUCCESS) {
        vkDestroyInstance(instance, nullptr);
        return 1;
    }
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, queueFamily, 0, &queue);
    // --8<-- [end:create-device]

    // --8<-- [start:destroy-device]
    vkDeviceWaitIdle(device); // nothing may still run on the GPU
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    // --8<-- [end:destroy-device]
    return 0;
}
