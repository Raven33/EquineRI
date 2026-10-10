// Canonical Vulkan: textures, views, dynamic rendering, a graphics pipeline,
// clears, discard and timestamps, the same program as snippets/ri/textures.cpp
// written with Vulkan alone (no RI). Here the layouts are yours: every image
// is transitioned by hand with vkCmdPipelineBarrier2 (synchronization2).
// Shaders: snippets/hlsl/doc_triangle_vs.hlsl and doc_triangle_ps.hlsl,
// compiled with
//   dxc -T vs_6_6 -E main -spirv -fspv-target-env=vulkan1.3 -fvk-use-dx-layout
//       -fvk-bind-sampler-heap 0 1 -Fo doc_triangle_vs.spv doc_triangle_vs.hlsl
// (and ps_6_6 for the pixel shader).
//
// A stand-alone program linking the Vulkan loader. The docs build compiles it
// against RI's Vulkan headers, which define VK_NO_PROTOTYPES for volk, so the
// prototypes are turned back on here. Error handling is reduced to "stop on
// failure" to keep the example short.
#undef VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace {

void check(VkResult result, const char *what) {
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "%s failed (%d)\n", what, static_cast<int>(result));
        std::exit(1);
    }
}

std::vector<uint32_t> loadSpirv(const char *path) {
    std::ifstream file(path, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    std::vector<uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), words.size() * 4);
    return words;
}

VkShaderModule createModule(VkDevice device, const char *path) {
    const std::vector<uint32_t> spirv = loadSpirv(path);
    const VkShaderModuleCreateInfo info{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                        .codeSize = spirv.size() * 4,
                                        .pCode = spirv.data()};
    VkShaderModule module = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device, &info, nullptr, &module), "vkCreateShaderModule");
    return module;
}

uint32_t findMemoryType(VkPhysicalDevice gpu, uint32_t allowedTypes, VkMemoryPropertyFlags required) {
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(gpu, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((allowedTypes & (1u << i)) != 0 && (memory.memoryTypes[i].propertyFlags & required) == required) {
            return i;
        }
    }
    std::fprintf(stderr, "no suitable memory type\n");
    std::exit(1);
}

VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment) { return (value + alignment - 1) / alignment * alignment; }

// --8<-- [start:format-supported]
// Whether a format has every feature a use needs, with optimal tiling.
bool formatSupports(VkPhysicalDevice gpu, VkFormat format, VkFormatFeatureFlags2 needed) {
    VkFormatProperties3 properties3{.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
    VkFormatProperties2 properties{.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &properties3};
    vkGetPhysicalDeviceFormatProperties2(gpu, format, &properties);
    return (properties3.optimalTilingFeatures & needed) == needed;
}
// --8<-- [end:format-supported]

// --8<-- [start:create-image-function]
// A 2D image placed in `memory` at `offset`. `viewFormats`: the formats views
// may use; more than one needs MUTABLE_FORMAT, and listing them exactly lets
// the driver keep compression.
VkImage createImage(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkImageCreateInfo info,
                    const std::vector<VkFormat> &viewFormats) {
    const VkImageFormatListCreateInfo list{.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
                                           .viewFormatCount = static_cast<uint32_t>(viewFormats.size()),
                                           .pViewFormats = viewFormats.data()};
    if (viewFormats.size() > 1) {
        info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        info.pNext = &list;
    }
    VkImage image = VK_NULL_HANDLE;
    check(vkCreateImage(device, &info, nullptr, &image), "vkCreateImage");
    check(vkBindImageMemory(device, image, memory, offset), "vkBindImageMemory");
    return image;
}
// --8<-- [end:create-image-function]

// --8<-- [start:create-view-function]
// A view: a format, an aspect, a mip range and a layer range of the image.
VkImageView createView(VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspect, uint32_t baseMip,
                       uint32_t mipCount) {
    const VkImageViewCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {.aspectMask = aspect,
                             .baseMipLevel = baseMip,
                             .levelCount = mipCount,
                             .baseArrayLayer = 0,
                             .layerCount = 1},
    };
    VkImageView view = VK_NULL_HANDLE;
    check(vkCreateImageView(device, &info, nullptr, &view), "vkCreateImageView");
    return view;
}
// --8<-- [end:create-view-function]

// --8<-- [start:transition-function]
// One layout transition of a whole image (every mip and layer of `aspect`),
// with the stages and accesses on each side.
void transition(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout oldLayout,
                VkImageLayout newLayout, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    const VkImageMemoryBarrier2 barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = srcStage,
        .srcAccessMask = srcAccess,
        .dstStageMask = dstStage,
        .dstAccessMask = dstAccess,
        .oldLayout = oldLayout, // UNDEFINED: the contents may be thrown away
        .newLayout = newLayout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {.aspectMask = aspect,
                             .baseMipLevel = 0,
                             .levelCount = VK_REMAINING_MIP_LEVELS,
                             .baseArrayLayer = 0,
                             .layerCount = VK_REMAINING_ARRAY_LAYERS},
    };
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(cmd, &dependency);
}
// --8<-- [end:transition-function]

// The C++ mirror of the shaders' root data.
struct DrawRoot {
    float color[4];
    float depth;
};

constexpr VkPipelineStageFlags2 kDepthStages =
    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;

} // namespace

int main(int argc, char **argv) {
    const char *vertexPath = argc > 2 ? argv[1] : "doc_triangle_vs.spv";
    const char *pixelPath = argc > 2 ? argv[2] : "doc_triangle_ps.spv";

    // Instance and GPU: see the device-creation snippet (snippets/vulkan/device.cpp).
    const VkApplicationInfo appInfo{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    const VkInstanceCreateInfo instanceInfo{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                            .pApplicationInfo = &appInfo};
    VkInstance instance = VK_NULL_HANDLE;
    check(vkCreateInstance(&instanceInfo, nullptr, &instance), "vkCreateInstance");
    uint32_t gpuCount = 1;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    vkEnumeratePhysicalDevices(instance, &gpuCount, &gpu); // the first GPU, assumed to have queue family 0
    if (gpu == VK_NULL_HANDLE) {
        return 1;
    }
    const uint32_t queueFamily = 0;

    // --8<-- [start:create-device]
    // What this program needs beyond Vulkan 1.3's defaults.
    VkPhysicalDeviceVulkan13Features v13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    v13.synchronization2 = VK_TRUE; // vkCmdPipelineBarrier2, vkCmdWriteTimestamp2
    v13.dynamicRendering = VK_TRUE; // vkCmdBeginRendering: no render pass objects
    VkPhysicalDeviceVulkan12Features v12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                         .pNext = &v13};
    v12.separateDepthStencilLayouts = VK_TRUE; // depth-only layouts for depth-only views
    v12.hostQueryReset = VK_TRUE;              // vkResetQueryPool from the CPU
    // --8<-- [end:create-device]
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queueInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                            .queueFamilyIndex = queueFamily,
                                            .queueCount = 1,
                                            .pQueuePriorities = &priority};
    const VkDeviceCreateInfo deviceInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                        .pNext = &v12,
                                        .queueCreateInfoCount = 1,
                                        .pQueueCreateInfos = &queueInfo};
    VkDevice device = VK_NULL_HANDLE;
    check(vkCreateDevice(gpu, &deviceInfo, nullptr, &device), "vkCreateDevice");
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    constexpr uint32_t kWidth = 1280;
    constexpr uint32_t kHeight = 720;

    // --8<-- [start:check-format]
    // D24S8 is missing on AMD: ask before using it.
    const VkFormat depthFormat =
        formatSupports(gpu, VK_FORMAT_D24_UNORM_S8_UINT,
                       VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT)
            ? VK_FORMAT_D24_UNORM_S8_UINT
            : VK_FORMAT_D32_SFLOAT_S8_UINT;
    // Sample counts are a per-image question: vkGetPhysicalDeviceImageFormatProperties2.
    // --8<-- [end:check-format]

    // --8<-- [start:image-info]
    const VkImageCreateInfo colorInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {kWidth, kHeight, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, // the only choice for optimal tiling
    };
    VkImageCreateInfo depthInfo = colorInfo;
    depthInfo.format = depthFormat;
    depthInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const std::vector<VkFormat> colorFormats = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB};
    // --8<-- [end:image-info]

    // --8<-- [start:requirements]
    // Size, alignment and allowed memory types of an image, without creating it (Vulkan 1.3).
    // (With a format list, chain it here too: requirements may depend on it.)
    const VkDeviceImageMemoryRequirements query{.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,
                                                .pCreateInfo = &colorInfo};
    VkMemoryRequirements2 colorReq{.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    vkGetDeviceImageMemoryRequirements(device, &query, &colorReq);
    const VkDeviceImageMemoryRequirements depthQuery{.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,
                                                     .pCreateInfo = &depthInfo};
    VkMemoryRequirements2 depthReq{.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    vkGetDeviceImageMemoryRequirements(device, &depthQuery, &depthReq);
    const VkDeviceSize depthOffset = alignUp(colorReq.memoryRequirements.size, depthReq.memoryRequirements.alignment);
    // memoryTypeBits: the memory types that can hold the image (usually device-local only).
    const uint32_t allowedTypes =
        colorReq.memoryRequirements.memoryTypeBits & depthReq.memoryRequirements.memoryTypeBits;
    // --8<-- [end:requirements]

    // --8<-- [start:create-textures]
    const VkMemoryAllocateInfo allocateInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = depthOffset + depthReq.memoryRequirements.size,
        .memoryTypeIndex = findMemoryType(gpu, allowedTypes, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    VkDeviceMemory memory = VK_NULL_HANDLE;
    check(vkAllocateMemory(device, &allocateInfo, nullptr, &memory), "vkAllocateMemory");
    VkImage color = createImage(device, memory, 0, colorInfo, colorFormats);
    VkImage depth = createImage(device, memory, depthOffset, depthInfo, {});
    // --8<-- [end:create-textures]

    // --8<-- [start:create-views]
    // An image has no view of its own: create every view you use.
    VkImageView colorView = createView(device, color, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1);
    VkImageView colorSrgb = createView(device, color, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1);
    VkImageView depthView =
        createView(device, depth, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1);
    // Shaders read one aspect: a depth-only view.
    VkImageView depthOnly = createView(device, depth, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1);
    // --8<-- [end:create-views]

    // --8<-- [start:create-pipeline]
    // The layout: push constants of the root data's size (a real renderer also
    // adds its descriptor set layouts, as in snippets/vulkan/compute.cpp).
    const VkPushConstantRange pushRange{.stageFlags = VK_SHADER_STAGE_ALL, .offset = 0, .size = sizeof(DrawRoot)};
    const VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                .pushConstantRangeCount = 1,
                                                .pPushConstantRanges = &pushRange};
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    check(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout), "vkCreatePipelineLayout");

    VkShaderModule vertex = createModule(device, vertexPath);
    VkShaderModule pixel = createModule(device, pixelPath);
    const VkPipelineShaderStageCreateInfo stages[] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vertex,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = pixel,
         .pName = "main"},
    };
    // Dynamic rendering: the attachment formats replace the render pass.
    const VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    const VkPipelineRenderingCreateInfo renderingInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &colorFormat,
        .depthAttachmentFormat = depthFormat,
        .stencilAttachmentFormat = depthFormat, // the format has stencil
    };
    // No vertex buffers: vertices are pulled in the shader (SV_VertexID).
    const VkPipelineVertexInputStateCreateInfo vertexInput{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    const VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    const VkPipelineViewportStateCreateInfo viewportState{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    const VkPipelineRasterizationStateCreateInfo rasterization{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_BACK_BIT,
        .frontFace = VK_FRONT_FACE_CLOCKWISE, // with the flipped viewport below, the same winding as D3D12
        .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo multisample{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    const VkPipelineDepthStencilStateCreateInfo depthState{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS,
    };
    const VkPipelineColorBlendAttachmentState blendAttachment{
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                          VK_COLOR_COMPONENT_A_BIT};
    const VkPipelineColorBlendStateCreateInfo blend{.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                    .attachmentCount = 1,
                                                    .pAttachments = &blendAttachment};
    // Viewport and scissor are set while recording, so one pipeline fits any size.
    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic{.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                   .dynamicStateCount = 2,
                                                   .pDynamicStates = dynamicStates};
    const VkGraphicsPipelineCreateInfo pipelineInfo{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &renderingInfo,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertexInput,
        .pInputAssemblyState = &inputAssembly,
        .pViewportState = &viewportState,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depthState,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic,
        .layout = pipelineLayout,
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline),
          "vkCreateGraphicsPipelines");
    vkDestroyShaderModule(device, vertex, nullptr); // not needed once the pipeline exists
    vkDestroyShaderModule(device, pixel, nullptr);
    // --8<-- [end:create-pipeline]

    // --8<-- [start:create-query-pool]
    const VkQueryPoolCreateInfo queryInfo{
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 4};
    VkQueryPool queries = VK_NULL_HANDLE;
    check(vkCreateQueryPool(device, &queryInfo, nullptr, &queries), "vkCreateQueryPool");
    vkResetQueryPool(device, queries, 0, 4); // queries start unusable until reset (hostQueryReset)
    // Ticks to seconds: timestampPeriod is nanoseconds per tick.
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(gpu, &properties);
    const double ticksPerSecond = 1e9 / static_cast<double>(properties.limits.timestampPeriod);
    // --8<-- [end:create-query-pool]

    // A readback buffer for the results (see snippets/vulkan/compute.cpp for buffers).
    const VkBufferCreateInfo resultInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                        .size = 4 * sizeof(uint64_t),
                                        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer ticks = VK_NULL_HANDLE;
    check(vkCreateBuffer(device, &resultInfo, nullptr, &ticks), "vkCreateBuffer");
    VkMemoryRequirements ticksReq{};
    vkGetBufferMemoryRequirements(device, ticks, &ticksReq);
    const VkMemoryAllocateInfo readbackInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = ticksReq.size,
        .memoryTypeIndex = findMemoryType(gpu, ticksReq.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory readback = VK_NULL_HANDLE;
    check(vkAllocateMemory(device, &readbackInfo, nullptr, &readback), "vkAllocateMemory");
    check(vkBindBufferMemory(device, ticks, readback, 0), "vkBindBufferMemory");
    void *ticksMapped = nullptr;
    check(vkMapMemory(device, readback, 0, VK_WHOLE_SIZE, 0, &ticksMapped), "vkMapMemory");

    const VkCommandPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                           .queueFamilyIndex = queueFamily};
    VkCommandPool commandPool = VK_NULL_HANDLE;
    check(vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool), "vkCreateCommandPool");
    const VkCommandBufferAllocateInfo commandInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                  .commandPool = commandPool,
                                                  .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                  .commandBufferCount = 1};
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(device, &commandInfo, &cmd), "vkAllocateCommandBuffers");
    const VkCommandBufferBeginInfo beginInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(vkBeginCommandBuffer(cmd, &beginInfo), "vkBeginCommandBuffer");

    // --8<-- [start:write-timestamp-begin]
    // ALL_COMMANDS: written once every earlier command has finished.
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries, 0);
    // --8<-- [end:write-timestamp-begin]

    // --8<-- [start:rendering]
    // --8<-- [start:begin-rendering]
    // Into attachment layouts. The old layout is UNDEFINED because both are
    // cleared: their previous contents are not needed.
    transition(cmd, color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    transition(cmd, depth, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
               VK_ACCESS_2_NONE, kDepthStages,
               VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo colorAttachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = colorView,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    };
    colorAttachment.clearValue.color = {{0.1f, 0.1f, 0.1f, 1.0f}};
    VkRenderingAttachmentInfo depthAttachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = depthView,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    };
    depthAttachment.clearValue.depthStencil = {.depth = 1.0f, .stencil = 0};
    const VkRenderingInfo rendering{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {.offset = {0, 0}, .extent = {kWidth, kHeight}},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colorAttachment,
        .pDepthAttachment = &depthAttachment,
        .pStencilAttachment = &depthAttachment, // the same view: its stencil aspect
    };
    vkCmdBeginRendering(cmd, &rendering);

    // A negative height flips y, so clip space points up as in D3D12.
    const VkViewport viewport{.x = 0.0f,
                              .y = static_cast<float>(kHeight),
                              .width = static_cast<float>(kWidth),
                              .height = -static_cast<float>(kHeight),
                              .minDepth = 0.0f,
                              .maxDepth = 1.0f};
    const VkRect2D scissor{.offset = {0, 0}, .extent = {kWidth, kHeight}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    // --8<-- [end:begin-rendering]
    // --8<-- [start:draw]
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const DrawRoot root{.color = {1.0f, 0.5f, 0.0f, 1.0f}, .depth = 0.5f};
    vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_ALL, 0, sizeof(root), &root);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    // --8<-- [end:draw]
    // --8<-- [start:end-rendering]
    vkCmdEndRendering(cmd);
    // Out of attachment layouts, into the layouts shaders read: this barrier
    // also makes the attachment writes visible to the pixel and compute shaders.
    transition(cmd, color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    transition(cmd, depth, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
               VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
               kDepthStages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | kDepthStages,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
    // --8<-- [end:end-rendering]
    // --8<-- [end:rendering]

    // --8<-- [start:write-descriptors]
    // A sampled image descriptor names the layout the image will be in when
    // the shader reads it (written into a heap set as in snippets/vulkan/compute.cpp).
    const VkDescriptorImageInfo sampled{.imageView = colorSrgb, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo sampledDepth{.imageView = depthOnly,
                                             .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    // --8<-- [end:write-descriptors]
    std::printf("descriptors for views %p and %p\n", static_cast<void *>(sampled.imageView),
                static_cast<void *>(sampledDepth.imageView));

    // --8<-- [start:read-only-depth]
    // Depth bound for testing while shaders sample it: a read-only layout,
    // usable by the attachment and by shaders at once. Nothing is stored.
    VkRenderingAttachmentInfo readOnlyDepth{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = depthView,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_NONE,
    };
    const VkRenderingInfo readOnlyRendering{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {.offset = {0, 0}, .extent = {kWidth, kHeight}},
        .layerCount = 1,
        .pDepthAttachment = &readOnlyDepth,
        .pStencilAttachment = &readOnlyDepth,
    };
    vkCmdBeginRendering(cmd, &readOnlyRendering);
    // ... draws with depthWriteEnable = VK_FALSE ...
    vkCmdEndRendering(cmd);
    // --8<-- [end:read-only-depth]

    // --8<-- [start:clears]
    // Outside a section, a color image is cleared as a transfer: TRANSFER_DST
    // layout (or GENERAL), then back to where shaders read it.
    transition(cmd, color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
               VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
    const VkImageSubresourceRange whole{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                        .baseMipLevel = 0,
                                        .levelCount = VK_REMAINING_MIP_LEVELS,
                                        .baseArrayLayer = 0,
                                        .layerCount = VK_REMAINING_ARRAY_LAYERS};
    vkCmdClearColorImage(cmd, color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &whole);
    transition(cmd, color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    // Buffers: vkCmdFillBuffer(cmd, buffer, 0, VK_WHOLE_SIZE, value), a transfer write too.
    // --8<-- [end:clears]

    // --8<-- [start:discard]
    // An image that takes over memory another resource used: a transition from
    // UNDEFINED drops the old contents and resets compression metadata. Required
    // before its first use, even if the next pass clears it.
    transition(cmd, color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
               VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    // --8<-- [end:discard]

    // --8<-- [start:write-timestamp-end]
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries, 1);
    // 64-bit results; WAIT: the copy waits until both timestamps are written.
    vkCmdCopyQueryPoolResults(cmd, queries, 0, 2, ticks, 0, sizeof(uint64_t),
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    // Make the copy visible to the CPU.
    const VkMemoryBarrier2 toHost{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                  .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                  .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                  .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                  .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    const VkDependencyInfo hostDependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &toHost};
    vkCmdPipelineBarrier2(cmd, &hostDependency);
    // --8<-- [end:write-timestamp-end]
    check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

    const VkCommandBufferSubmitInfo submitCommand{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                                  .commandBuffer = cmd};
    const VkSubmitInfo2 submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                               .commandBufferInfoCount = 1,
                               .pCommandBufferInfos = &submitCommand};
    check(vkQueueSubmit2(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit2");
    check(vkQueueWaitIdle(queue), "vkQueueWaitIdle"); // see snippets/vulkan/compute.cpp for timeline semaphores

    // --8<-- [start:read-timestamps]
    const auto *values = static_cast<const uint64_t *>(ticksMapped);
    std::printf("GPU time: %.3f ms\n", static_cast<double>(values[1] - values[0]) * 1000.0 / ticksPerSecond);
    // A query must be reset before it is written again: on the CPU, or with
    // vkCmdResetQueryPool in a command buffer (outside a render pass).
    vkResetQueryPool(device, queries, 0, 2);
    // --8<-- [end:read-timestamps]

    // --8<-- [start:destroy]
    vkDestroyCommandPool(device, commandPool, nullptr);
    vkDestroyQueryPool(device, queries, nullptr);
    vkDestroyBuffer(device, ticks, nullptr);
    vkFreeMemory(device, readback, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    for (VkImageView view : {colorView, colorSrgb, depthView, depthOnly}) {
        vkDestroyImageView(device, view, nullptr);
    }
    vkDestroyImage(device, color, nullptr);
    vkDestroyImage(device, depth, nullptr);
    vkFreeMemory(device, memory, nullptr); // after the images bound to it
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    // --8<-- [end:destroy]
    return 0;
}
