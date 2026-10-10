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

    // --8<-- [start:native-texture]
    // A texture's VkImage and the layout it rests in. Native code may move it
    // to another layout, but must return it to restingLayout before RI uses it.
    RI::Memory textureMemory = RI::allocateMemory(device, RI::MemoryKind::eGPU, 16u << 20);
    RI::Texture texture = RI::createTexture(device, textureMemory, 0,
                                            {.width = 256,
                                             .height = 256,
                                             .format = RI::Format::eRGBA8Unorm,
                                             .usage = RI::TextureUsage::eSampled | RI::TextureUsage::eCopyDst});
    const RI::Vk::NativeTexture image = RI::Vk::native(texture);
    const VkImageView view = RI::Vk::native(texture.view); // views: VkImageView

    const VkImageSubresourceRange whole{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const auto layoutChange = [&](VkImageLayout from, VkImageLayout to) {
        const VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                            .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                            .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                                            .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                            .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                            .oldLayout = from,
                                            .newLayout = to,
                                            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                            .image = image.image,
                                            .subresourceRange = whole};
        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
        n.functions->vkCmdPipelineBarrier2(RI::Vk::native(cmd), &dependency);
    };
    RI::resetCommandList(cmd);
    RI::cmdBegin(cmd);
    // Out of rest, a native clear, back to rest.
    layoutChange(image.restingLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearColorValue red{{1.0f, 0.0f, 0.0f, 1.0f}};
    n.functions->vkCmdClearColorImage(RI::Vk::native(cmd), image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &red, 1,
                                      &whole);
    layoutChange(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, image.restingLayout);
    RI::cmdEnd(cmd);
    RI::wait(device.graphicsQueue, RI::submit(device.graphicsQueue, cmd));
    std::printf("image view %p\n", static_cast<void *>(view));
    // --8<-- [end:native-texture]

    // --8<-- [start:native-query-pool]
    RI::QueryPool pool = RI::createQueryPool(device, {.type = RI::QueryType::eTimestamp, .count = 2});
    const VkQueryPool vkPool = RI::Vk::native(pool);
    // E.g. read results on the CPU without a buffer (after the timeline point).
    uint64_t results[2] = {};
    n.functions->vkGetQueryPoolResults(n.device, vkPool, 0, 2, sizeof(results), results, sizeof(uint64_t),
                                       VK_QUERY_RESULT_64_BIT);
    // --8<-- [end:native-query-pool]

    RI::destroyQueryPool(device, pool);
    RI::destroyTexture(device, texture);
    RI::freeMemory(device, textureMemory);
    RI::destroyCommandList(device, cmd);
    RI::destroyDescriptorHeap(device, heap);
    RI::destroyBuffer(device, buffer);
    RI::freeMemory(device, memory);

    RI::destroyDevice(device);

    // --8<-- [start:layout-mode]
    // Force a layout mode, to test or measure (P78). User code is the same in every mode.
    RI::Device modeT = RI::Vk::createDevice({.gpu = gpus[0]}, {.layoutMode = RI::Vk::LayoutMode::eTransitions});
    const RI::Vk::LayoutMode mode = RI::Vk::native(modeT).layoutMode; // the mode in use, never eAuto
    std::printf("layout mode %u\n", static_cast<unsigned>(mode));
    // --8<-- [end:layout-mode]
    RI::destroyDevice(modeT);
    return 0;
}
