// RI with Vulkan native access (P84, P92): change RI's device features, add
// an extension with its feature structure, and reach the native handles.
// Compiled against EquineVulkanBackend only.

#include <RI/RI.hpp>
#include <RI/Vk/Native.hpp>

#include <cstdio>
#include <vector>

int main() {
    const std::vector<RI::Gpu> gpus = RI::listGpus();
    if (gpus.empty()) {
        return 1;
    }

    // --8<-- [start:robustness]
    // Start from what RI would enable, change fields, pass the result back.
    RI::Vk::DeviceFeatures features = RI::Vk::defaultDeviceFeatures(gpus[0]);
    features.core.robustBufferAccess = VK_TRUE; // off by default
    features.v13.robustImageAccess = VK_TRUE;
    RI::Device robust = RI::Vk::createDevice({.gpu = gpus[0]}, {.features = &features});
    // --8<-- [end:robustness]
    RI::destroyDevice(robust);

    // --8<-- [start:extension]
    // An extension RI does not enable, with its feature structure. RI appends
    // the structure after its own chain and leaves it untouched.
    const char *const extensions[] = {VK_KHR_SHADER_CLOCK_EXTENSION_NAME};
    VkPhysicalDeviceShaderClockFeaturesKHR clock{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR,
        .shaderSubgroupClock = VK_TRUE,
    };
    RI::Device device = RI::Vk::createDevice({.gpu = gpus[0]}, {.extensions = extensions, .pNext = &clock});
    // --8<-- [end:extension]

    // --8<-- [start:native-handles]
    const RI::Vk::NativeDevice n = RI::Vk::native(device);
    // Device-level functions: through the device's own table.
    n.functions->vkDeviceWaitIdle(n.device);
    // Core instance-level functions: volk's globals.
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(n.physicalDevice, &properties);
    std::printf("Vulkan %u.%u on queue family %u\n", VK_API_VERSION_MAJOR(n.apiVersion),
                VK_API_VERSION_MINOR(n.apiVersion), n.graphicsQueueFamily);
    // --8<-- [end:native-handles]

    RI::destroyDevice(device);
    return 0;
}
