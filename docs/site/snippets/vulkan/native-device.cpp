// RI with Vulkan native access (P84, P92): change RI's device features, add
// an extension with its feature structure, and reach the native handles.
// Compiled against EquineVulkanBackend only.

#include <RI/RI.hpp>
#include <RI/Vk/Native.hpp>

#include <cstdint>
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

    // --8<-- [start:object-handles]
    RI::Memory memory = RI::allocateMemory(device, RI::MemoryKind::eGPU, 65536);
    RI::Buffer buffer = RI::createBuffer(device, memory, 0, {.size = 1024, .usage = RI::BufferUsage::eCopyDst});
    RI::DescriptorHeap heap = RI::createDescriptorHeap(device, {.resourceCount = 16, .samplerCount = 16});
    const VkBuffer vkBuffer = RI::Vk::native(buffer);
    const VkDeviceMemory vkMemory = RI::Vk::native(memory);
    const RI::Vk::NativeDescriptorHeap vkHeap = RI::Vk::native(heap); // set 0: views, set 1: samplers
    std::printf("memory %p, heap sets %p / %p\n", static_cast<void *>(vkMemory), static_cast<void *>(vkHeap.resourceSet),
                static_cast<void *>(vkHeap.samplerSet));

    // Name the buffer for debuggers and validation messages. Debug-utils
    // functions are instance extension functions: fetch them per instance.
    const auto setName = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
        vkGetInstanceProcAddr(n.instance, "vkSetDebugUtilsObjectNameEXT"));
    const VkDebugUtilsObjectNameInfoEXT nameInfo{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        .objectType = VK_OBJECT_TYPE_BUFFER,
        .objectHandle = reinterpret_cast<uint64_t>(vkBuffer),
        .pObjectName = "particles",
    };
    setName(n.device, &nameInfo);
    // --8<-- [end:object-handles]

    // --8<-- [start:native-command]
    RI::CommandList cmd = RI::createCommandList(device);
    RI::cmdBegin(cmd);
    // A native command recorded into RI's command buffer, through the device's
    // function table. vkCmdFillBuffer changes no state RI relies on.
    n.functions->vkCmdFillBuffer(RI::Vk::native(cmd), vkBuffer, 0, VK_WHOLE_SIZE, 0);
    RI::cmdEnd(cmd);
    RI::wait(device.graphicsQueue, RI::submit(device.graphicsQueue, cmd));
    // --8<-- [end:native-command]

    RI::destroyCommandList(device, cmd);
    RI::destroyDescriptorHeap(device, heap);
    RI::destroyBuffer(device, buffer);
    RI::freeMemory(device, memory);

    RI::destroyDevice(device);
    return 0;
}
