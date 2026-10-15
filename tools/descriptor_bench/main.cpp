// The P94 measurement: RI's Vulkan descriptor heaps (descriptor sets with
// update-after-bind mutable descriptors, as DXC's emulated heap expects)
// against VK_EXT_descriptor_heap.
//
// Raw Vulkan, no RI: two devices on the same GPU, one per mechanism, with the
// same textures, buffers and heap contents. Rounds alternate between them so
// clock changes hit both alike. Measured:
//   - GPU time of four compute workloads that fetch through the heap
//     (bench.comp: random texture loads, wave-uniform texture loads, random
//     buffer loads, random texture + sampler);
//   - CPU time to write descriptors (vkUpdateDescriptorSets against
//     vkWriteResourceDescriptorsEXT), in batches as RI's writeDescriptors does;
//   - CPU time to record pipeline + root data + dispatch.
// Shaders come from glslang (DXC's descriptor-heap output is incomplete), so
// both mechanisms run the same source.
//
// Usage: descriptor_bench [--gpu N] [--rounds N] [--validate]

#include <volk.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

#define VK(call)                                                                                                       \
    do {                                                                                                               \
        const VkResult result_ = (call);                                                                               \
        if (result_ != VK_SUCCESS) {                                                                                   \
            std::fprintf(stderr, "%s failed: %d (line %d)\n", #call, int(result_), __LINE__);                          \
            std::exit(1);                                                                                              \
        }                                                                                                              \
    } while (0)

constexpr uint32_t kSlots = 65536;          // resource heap slots: textures in the first half, buffers in the second
constexpr uint32_t kHalf = kSlots / 2;
constexpr uint32_t kTextures = 256;         // distinct 64x64 textures behind the texture slots
constexpr uint32_t kBuffers = 256;          // distinct 4 KiB buffer ranges behind the buffer slots
constexpr uint32_t kSamplers = 16;
constexpr uint32_t kThreads = 1u << 20;
constexpr uint32_t kFetches = 32;
constexpr uint32_t kBatch = 1024;           // descriptors per write call
const char *const kWorkloads[] = {"TEX_RANDOM", "TEX_UNIFORM", "BUF_RANDOM", "SAMPLED"};
constexpr uint32_t kWorkloadCount = 4;

struct Root {
    VkDeviceAddress dst;
    uint32_t count, slotMask, textureScale, bufferScale, bufferBase, samplerMask;
};

double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[v.size() / 2];
}

std::vector<uint32_t> readSpirv(const std::string &path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::fprintf(stderr, "cannot read %s\n", path.c_str());
        std::exit(1);
    }
    std::vector<uint32_t> words(size_t(file.tellg()) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char *>(words.data()), std::streamsize(words.size() * 4));
    return words;
}

struct Heap {
    VkPhysicalDeviceDescriptorHeapPropertiesEXT properties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT};
    PFN_vkWriteResourceDescriptorsEXT writeResources = nullptr;
    PFN_vkWriteSamplerDescriptorsEXT writeSamplers = nullptr;
    PFN_vkCmdBindResourceHeapEXT bindResources = nullptr;
    PFN_vkCmdBindSamplerHeapEXT bindSamplers = nullptr;
    PFN_vkCmdPushDataEXT pushData = nullptr;
    VkBuffer resourceBuffer = VK_NULL_HANDLE, samplerBuffer = VK_NULL_HANDLE;
    VkDeviceMemory resourceMemory = VK_NULL_HANDLE, samplerMemory = VK_NULL_HANDLE;
    std::byte *resources = nullptr, *samplers = nullptr; // mapped, at the aligned heap start
    VkBindHeapInfoEXT resourceBind{.sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT};
    VkBindHeapInfoEXT samplerBind{.sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT};
    uint64_t slotStride = 0;
};

struct Sets {
    VkDescriptorSetLayout layouts[2]{};
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet sets[2]{};
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    std::vector<VkSampler> samplers;
};

struct Bench {
    bool heapMode = false;
    const char *name = "";
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkPhysicalDeviceMemoryProperties memory{};
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;
    float timestampPeriod = 1.0f;
    std::vector<VkImage> images;
    std::vector<VkImageView> views; // emulated mode only
    VkDeviceMemory imageMemory = VK_NULL_HANDLE;
    VkBuffer data = VK_NULL_HANDLE, out = VK_NULL_HANDLE, readback = VK_NULL_HANDLE;
    VkDeviceMemory dataMemory = VK_NULL_HANDLE, outMemory = VK_NULL_HANDLE, readbackMemory = VK_NULL_HANDLE;
    VkDeviceAddress dataAddress = 0, outAddress = 0;
    void *readbackMapped = nullptr;
    VkPipeline pipelines[kWorkloadCount]{};
    Heap heap;
    Sets sets;
    uint32_t textureScale = 1, bufferScale = 1;
};

uint32_t memoryType(const Bench &b, uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid = 0) {
    for (uint32_t i = 0; i < b.memory.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags flags = b.memory.memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) && (flags & want) == want && !(flags & avoid)) {
            return i;
        }
    }
    return UINT32_MAX;
}

VkDeviceMemory allocate(const Bench &b, VkMemoryRequirements req, VkMemoryPropertyFlags want, bool address) {
    uint32_t type = memoryType(b, req.memoryTypeBits, want | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD);
    if (type == UINT32_MAX) {
        type = memoryType(b, req.memoryTypeBits, want, VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD);
    }
    const VkMemoryAllocateFlagsInfo flags{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                          .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    const VkMemoryAllocateInfo info{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                    .pNext = address ? &flags : nullptr,
                                    .allocationSize = req.size,
                                    .memoryTypeIndex = type};
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VK(vkAllocateMemory(b.device, &info, nullptr, &memory));
    return memory;
}

VkBuffer createBuffer(const Bench &b, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want,
                      VkDeviceMemory &memory, uint64_t alignment = 1) {
    const VkBufferCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size + alignment, .usage = usage};
    VkBuffer buffer = VK_NULL_HANDLE;
    VK(vkCreateBuffer(b.device, &info, nullptr, &buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(b.device, buffer, &req);
    memory = allocate(b, req, want, (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0);
    VK(vkBindBufferMemory(b.device, buffer, memory, 0));
    return buffer;
}

VkDeviceAddress addressOf(const Bench &b, VkBuffer buffer) {
    const VkBufferDeviceAddressInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffer};
    return vkGetBufferDeviceAddress(b.device, &info);
}

void begin(Bench &b) {
    VK(vkResetCommandPool(b.device, b.pool, 0));
    const VkCommandBufferBeginInfo info{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK(vkBeginCommandBuffer(b.cmd, &info));
}

void submitAndWait(Bench &b) {
    VK(vkEndCommandBuffer(b.cmd));
    const VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &b.cmd};
    VK(vkResetFences(b.device, 1, &b.fence));
    VK(vkQueueSubmit(b.queue, 1, &submit, b.fence));
    VK(vkWaitForFences(b.device, 1, &b.fence, VK_TRUE, UINT64_MAX));
}

void barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
             VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    const VkMemoryBarrier2 memoryBarrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                         .srcStageMask = srcStage,
                                         .srcAccessMask = srcAccess,
                                         .dstStageMask = dstStage,
                                         .dstAccessMask = dstAccess};
    const VkDependencyInfo info{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &memoryBarrier};
    vkCmdPipelineBarrier2(cmd, &info);
}

void createDevice(Bench &b, VkPhysicalDevice gpu) {
    b.gpu = gpu;
    vkGetPhysicalDeviceMemoryProperties(gpu, &b.memory);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(gpu, &properties);
    b.timestampPeriod = properties.limits.timestampPeriod;
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families.data());
    for (uint32_t i = 0; i < count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            b.family = i;
            break;
        }
    }
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                        .queueFamilyIndex = b.family,
                                        .queueCount = 1,
                                        .pQueuePriorities = &priority};

    VkPhysicalDeviceDescriptorHeapFeaturesEXT heapFeatures{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT, .descriptorHeap = VK_TRUE};
    VkPhysicalDeviceShaderUntypedPointersFeaturesKHR untyped{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNTYPED_POINTERS_FEATURES_KHR,
        .pNext = &heapFeatures,
        .shaderUntypedPointers = VK_TRUE};
    VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mutableFeatures{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT, .mutableDescriptorType = VK_TRUE};
    VkPhysicalDeviceVulkan14Features v14{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
                                         .pNext = b.heapMode ? static_cast<void *>(&untyped)
                                                             : static_cast<void *>(&mutableFeatures),
                                         .maintenance5 = VK_TRUE};
    VkPhysicalDeviceVulkan13Features v13{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &v14, .synchronization2 = VK_TRUE};
    VkPhysicalDeviceVulkan12Features v12{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &v13,
        .descriptorIndexing = VK_TRUE,
        .shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
        .shaderStorageBufferArrayNonUniformIndexing = VK_TRUE,
        .descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE,
        .descriptorBindingSampledImageUpdateAfterBind = VK_TRUE,
        .descriptorBindingStorageImageUpdateAfterBind = VK_TRUE,
        .descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE,
        .descriptorBindingUniformTexelBufferUpdateAfterBind = VK_TRUE,
        .descriptorBindingStorageTexelBufferUpdateAfterBind = VK_TRUE,
        .descriptorBindingPartiallyBound = VK_TRUE,
        .descriptorBindingVariableDescriptorCount = VK_TRUE,
        .runtimeDescriptorArray = VK_TRUE,
        .bufferDeviceAddress = VK_TRUE,
    };
    std::vector<const char *> extensions;
    if (b.heapMode) {
        extensions = {VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME, VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME};
    } else {
        extensions = {VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME};
    }
    const VkDeviceCreateInfo info{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                  .pNext = &v12,
                                  .queueCreateInfoCount = 1,
                                  .pQueueCreateInfos = &queue,
                                  .enabledExtensionCount = uint32_t(extensions.size()),
                                  .ppEnabledExtensionNames = extensions.data()};
    VK(vkCreateDevice(gpu, &info, nullptr, &b.device));
    vkGetDeviceQueue(b.device, b.family, 0, &b.queue);

    const VkCommandPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                           .queueFamilyIndex = b.family};
    VK(vkCreateCommandPool(b.device, &poolInfo, nullptr, &b.pool));
    const VkCommandBufferAllocateInfo cmdInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                              .commandPool = b.pool,
                                              .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                              .commandBufferCount = 1};
    VK(vkAllocateCommandBuffers(b.device, &cmdInfo, &b.cmd));
    const VkFenceCreateInfo fenceInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK(vkCreateFence(b.device, &fenceInfo, nullptr, &b.fence));
    const VkQueryPoolCreateInfo queryInfo{
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2};
    VK(vkCreateQueryPool(b.device, &queryInfo, nullptr, &b.queries));
}

// Textures j = 0..255 hold the color (j/255, 1 - j/255, j%7/7, 1); buffer
// range j holds the float j + 1 in every component.
void createResources(Bench &b) {
    const VkImageCreateInfo imageInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                      .imageType = VK_IMAGE_TYPE_2D,
                                      .format = VK_FORMAT_R8G8B8A8_UNORM,
                                      .extent = {64, 64, 1},
                                      .mipLevels = 1,
                                      .arrayLayers = 1,
                                      .samples = VK_SAMPLE_COUNT_1_BIT,
                                      .tiling = VK_IMAGE_TILING_OPTIMAL,
                                      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    b.images.resize(kTextures);
    VkMemoryRequirements req{};
    for (VkImage &image : b.images) {
        VK(vkCreateImage(b.device, &imageInfo, nullptr, &image));
    }
    vkGetImageMemoryRequirements(b.device, b.images[0], &req);
    const VkDeviceSize step = (req.size + req.alignment - 1) / req.alignment * req.alignment;
    VkMemoryRequirements all = req;
    all.size = step * kTextures;
    b.imageMemory = allocate(b, all, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false);
    for (uint32_t j = 0; j < kTextures; ++j) {
        VK(vkBindImageMemory(b.device, b.images[j], b.imageMemory, step * j));
    }
    if (!b.heapMode) {
        b.views.resize(kTextures);
        for (uint32_t j = 0; j < kTextures; ++j) {
            const VkImageViewCreateInfo viewInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                                 .image = b.images[j],
                                                 .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                                 .format = VK_FORMAT_R8G8B8A8_UNORM,
                                                 .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            VK(vkCreateImageView(b.device, &viewInfo, nullptr, &b.views[j]));
        }
    }
    const VkBufferUsageFlags dataUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    b.data = createBuffer(b, kBuffers * 4096ull, dataUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, b.dataMemory);
    b.dataAddress = addressOf(b, b.data);
    b.out = createBuffer(b, kThreads * 16ull,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, b.outMemory);
    b.outAddress = addressOf(b, b.out);
    b.readback = createBuffer(b, kThreads * 16ull, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              b.readbackMemory);
    VK(vkMapMemory(b.device, b.readbackMemory, 0, VK_WHOLE_SIZE, 0, &b.readbackMapped));

    begin(b);
    std::vector<VkImageMemoryBarrier2> toDst(kTextures), toRead(kTextures);
    for (uint32_t j = 0; j < kTextures; ++j) {
        toDst[j] = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                    .dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT,
                    .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    .image = b.images[j],
                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        toRead[j] = toDst[j];
        toRead[j].srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        toRead[j].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toRead[j].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        toRead[j].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        toRead[j].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead[j].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                         .imageMemoryBarrierCount = kTextures,
                         .pImageMemoryBarriers = toDst.data()};
    vkCmdPipelineBarrier2(b.cmd, &dep);
    for (uint32_t j = 0; j < kTextures; ++j) {
        const VkClearColorValue color{.float32 = {j / 255.0f, 1.0f - j / 255.0f, float(j % 7) / 7.0f, 1.0f}};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(b.cmd, b.images[j], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    }
    for (uint32_t j = 0; j < kBuffers; ++j) {
        const float value = float(j + 1);
        uint32_t bits;
        std::memcpy(&bits, &value, 4);
        vkCmdFillBuffer(b.cmd, b.data, j * 4096ull, 4096, bits);
    }
    dep.pImageMemoryBarriers = toRead.data();
    vkCmdPipelineBarrier2(b.cmd, &dep);
    barrier(b.cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    submitAndWait(b);
}

VkSamplerCreateInfo samplerInfo(uint32_t i) {
    return {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = (i & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
            .minFilter = (i & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = (i & 2) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeV = (i & 4) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .maxLod = 1000.0f};
}

// RI's layout (backends/vulkan/Device.cpp, createHeapLayouts): set 0 binding 0
// mutable resource descriptors, set 1 binding 0 samplers, both update-after-bind,
// partially bound, variable count.
void createSets(Bench &b) {
    const VkDescriptorType mutableTypes[] = {
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    };
    const VkMutableDescriptorTypeListEXT list{.descriptorTypeCount = 6, .pDescriptorTypes = mutableTypes};
    const VkMutableDescriptorTypeCreateInfoEXT mutableInfo{
        .sType = VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT,
        .mutableDescriptorTypeListCount = 1,
        .pMutableDescriptorTypeLists = &list};
    const VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                           VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                           VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
    for (uint32_t s = 0; s < 2; ++s) {
        const VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
            .pNext = s == 0 ? &mutableInfo : nullptr,
            .bindingCount = 1,
            .pBindingFlags = &flags};
        const VkDescriptorSetLayoutBinding binding{
            .binding = 0,
            .descriptorType = s == 0 ? VK_DESCRIPTOR_TYPE_MUTABLE_EXT : VK_DESCRIPTOR_TYPE_SAMPLER,
            .descriptorCount = s == 0 ? kSlots : 2048,
            .stageFlags = VK_SHADER_STAGE_ALL};
        const VkDescriptorSetLayoutCreateInfo info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                   .pNext = &flagsInfo,
                                                   .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
                                                   .bindingCount = 1,
                                                   .pBindings = &binding};
        VK(vkCreateDescriptorSetLayout(b.device, &info, nullptr, &b.sets.layouts[s]));
    }
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_MUTABLE_EXT, kSlots},
                                          {VK_DESCRIPTOR_TYPE_SAMPLER, kSamplers}};
    const VkMutableDescriptorTypeListEXT poolLists[] = {list, {}};
    const VkMutableDescriptorTypeCreateInfoEXT poolMutable{
        .sType = VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT,
        .mutableDescriptorTypeListCount = 2,
        .pMutableDescriptorTypeLists = poolLists};
    const VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                              .pNext = &poolMutable,
                                              .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
                                              .maxSets = 2,
                                              .poolSizeCount = 2,
                                              .pPoolSizes = sizes};
    VK(vkCreateDescriptorPool(b.device, &poolInfo, nullptr, &b.sets.pool));
    const uint32_t counts[] = {kSlots, kSamplers};
    for (uint32_t s = 0; s < 2; ++s) {
        const VkDescriptorSetVariableDescriptorCountAllocateInfo variable{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,
            .descriptorSetCount = 1,
            .pDescriptorCounts = &counts[s]};
        const VkDescriptorSetAllocateInfo info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                               .pNext = &variable,
                                               .descriptorPool = b.sets.pool,
                                               .descriptorSetCount = 1,
                                               .pSetLayouts = &b.sets.layouts[s]};
        VK(vkAllocateDescriptorSets(b.device, &info, &b.sets.sets[s]));
    }
    const VkPushConstantRange range{VK_SHADER_STAGE_ALL, 0, sizeof(Root)};
    const VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                .setLayoutCount = 2,
                                                .pSetLayouts = b.sets.layouts,
                                                .pushConstantRangeCount = 1,
                                                .pPushConstantRanges = &range};
    VK(vkCreatePipelineLayout(b.device, &layoutInfo, nullptr, &b.sets.pipelineLayout));
    b.sets.samplers.resize(kSamplers);
    std::vector<VkDescriptorImageInfo> images(kSamplers);
    for (uint32_t i = 0; i < kSamplers; ++i) {
        const VkSamplerCreateInfo info = samplerInfo(i);
        VK(vkCreateSampler(b.device, &info, nullptr, &b.sets.samplers[i]));
        images[i].sampler = b.sets.samplers[i];
    }
    const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                     .dstSet = b.sets.sets[1],
                                     .descriptorCount = kSamplers,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                                     .pImageInfo = images.data()};
    vkUpdateDescriptorSets(b.device, 1, &write, 0, nullptr);
}

// The resource heap holds kSlots slots of max(image, buffer) descriptor size,
// like a D3D12 heap, plus the driver's reserved range.
void createHeaps(Bench &b) {
    Heap &h = b.heap;
    VkPhysicalDeviceProperties2 properties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                           .pNext = &h.properties};
    vkGetPhysicalDeviceProperties2(b.gpu, &properties);
    const auto load = [&](const char *name) {
        PFN_vkVoidFunction f = vkGetDeviceProcAddr(b.device, name);
        if (!f) {
            std::fprintf(stderr, "missing %s\n", name);
            std::exit(1);
        }
        return f;
    };
    h.writeResources = reinterpret_cast<PFN_vkWriteResourceDescriptorsEXT>(load("vkWriteResourceDescriptorsEXT"));
    h.writeSamplers = reinterpret_cast<PFN_vkWriteSamplerDescriptorsEXT>(load("vkWriteSamplerDescriptorsEXT"));
    h.bindResources = reinterpret_cast<PFN_vkCmdBindResourceHeapEXT>(load("vkCmdBindResourceHeapEXT"));
    h.bindSamplers = reinterpret_cast<PFN_vkCmdBindSamplerHeapEXT>(load("vkCmdBindSamplerHeapEXT"));
    h.pushData = reinterpret_cast<PFN_vkCmdPushDataEXT>(load("vkCmdPushDataEXT"));

    const VkPhysicalDeviceDescriptorHeapPropertiesEXT &p = h.properties;
    const auto alignUp = [](uint64_t v, uint64_t a) { return a ? (v + a - 1) / a * a : v; };
    h.slotStride = alignUp(std::max(p.imageDescriptorSize, p.bufferDescriptorSize),
                           std::max(p.imageDescriptorAlignment, p.bufferDescriptorAlignment));
    b.textureScale = uint32_t(h.slotStride / p.imageDescriptorSize);
    b.bufferScale = uint32_t(h.slotStride / p.bufferDescriptorSize);
    if (b.textureScale * p.imageDescriptorSize != h.slotStride || b.bufferScale * p.bufferDescriptorSize != h.slotStride) {
        std::fprintf(stderr, "descriptor sizes do not divide the slot stride\n");
        std::exit(1);
    }
    const auto make = [&](uint64_t used, uint64_t reserved, uint64_t alignment, VkBuffer &buffer,
                          VkDeviceMemory &memory, std::byte *&mapped, VkBindHeapInfoEXT &bind) {
        const uint64_t reservedOffset = alignUp(used, std::max<uint64_t>(alignment, 256));
        const uint64_t size = reservedOffset + std::max<uint64_t>(reserved, 1);
        buffer = createBuffer(b, size, VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, memory,
                              alignment);
        void *base = nullptr;
        VK(vkMapMemory(b.device, memory, 0, VK_WHOLE_SIZE, 0, &base));
        const VkDeviceAddress address = addressOf(b, buffer);
        const VkDeviceAddress aligned = alignUp(address, alignment);
        mapped = static_cast<std::byte *>(base) + (aligned - address);
        bind.heapRange = {aligned, size};
        bind.reservedRangeOffset = reservedOffset;
        bind.reservedRangeSize = reserved;
    };
    make(kSlots * h.slotStride, p.minResourceHeapReservedRange, p.resourceHeapAlignment, h.resourceBuffer,
         h.resourceMemory, h.resources, h.resourceBind);
    make(kSamplers * alignUp(p.samplerDescriptorSize, p.samplerDescriptorAlignment), p.minSamplerHeapReservedRange,
         p.samplerHeapAlignment, h.samplerBuffer, h.samplerMemory, h.samplers, h.samplerBind);

    std::vector<VkSamplerCreateInfo> infos(kSamplers);
    std::vector<VkHostAddressRangeEXT> ranges(kSamplers);
    const uint64_t samplerStride = alignUp(p.samplerDescriptorSize, p.samplerDescriptorAlignment);
    for (uint32_t i = 0; i < kSamplers; ++i) {
        infos[i] = samplerInfo(i);
        ranges[i] = {h.samplers + i * samplerStride, size_t(p.samplerDescriptorSize)};
    }
    VK(h.writeSamplers(b.device, kSamplers, infos.data(), ranges.data()));
}

// Writes every resource slot: textures in the first half, buffer ranges in
// the second. Returns the seconds spent in the write calls only.
double writeAll(Bench &b) {
    double seconds = 0.0;
    if (!b.heapMode) {
        std::vector<VkDescriptorImageInfo> images(kHalf);
        std::vector<VkDescriptorBufferInfo> buffers(kHalf);
        std::vector<VkWriteDescriptorSet> writes(kSlots);
        for (uint32_t s = 0; s < kSlots; ++s) {
            VkWriteDescriptorSet &w = writes[s];
            w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = b.sets.sets[0], .dstArrayElement = s,
                 .descriptorCount = 1};
            if (s < kHalf) {
                images[s] = {.imageView = b.views[s % kTextures], .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                w.pImageInfo = &images[s];
            } else {
                buffers[s - kHalf] = {b.data, ((s - kHalf) % kBuffers) * 4096ull, 4096};
                w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                w.pBufferInfo = &buffers[s - kHalf];
            }
        }
        const double start = now();
        for (uint32_t s = 0; s < kSlots; s += kBatch) {
            vkUpdateDescriptorSets(b.device, kBatch, &writes[s], 0, nullptr);
        }
        seconds = now() - start;
    } else {
        Heap &h = b.heap;
        std::vector<VkImageViewCreateInfo> views(kHalf);
        std::vector<VkImageDescriptorInfoEXT> images(kHalf);
        std::vector<VkDeviceAddressRangeEXT> addresses(kHalf);
        std::vector<VkResourceDescriptorInfoEXT> infos(kSlots);
        std::vector<VkHostAddressRangeEXT> ranges(kSlots);
        for (uint32_t s = 0; s < kSlots; ++s) {
            infos[s] = {.sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT};
            if (s < kHalf) {
                views[s] = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                            .image = b.images[s % kTextures],
                            .viewType = VK_IMAGE_VIEW_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
                images[s] = {.sType = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT,
                             .pView = &views[s],
                             .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                infos[s].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                infos[s].data.pImage = &images[s];
                ranges[s] = {h.resources + s * h.slotStride, size_t(h.properties.imageDescriptorSize)};
            } else {
                addresses[s - kHalf] = {b.dataAddress + ((s - kHalf) % kBuffers) * 4096ull, 4096};
                infos[s].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                infos[s].data.pAddressRange = &addresses[s - kHalf];
                ranges[s] = {h.resources + s * h.slotStride, size_t(h.properties.bufferDescriptorSize)};
            }
        }
        const double start = now();
        for (uint32_t s = 0; s < kSlots; s += kBatch) {
            VK(h.writeResources(b.device, kBatch, &infos[s], &ranges[s]));
        }
        seconds = now() - start;
    }
    return seconds;
}

void createPipelines(Bench &b, const std::string &dir) {
    for (uint32_t w = 0; w < kWorkloadCount; ++w) {
        const std::string path = dir + "/bench_" + kWorkloads[w] + (b.heapMode ? "_heap" : "_sets") + ".spv";
        const std::vector<uint32_t> code = readSpirv(path);
        const VkShaderModuleCreateInfo moduleInfo{
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = code.size() * 4, .pCode = code.data()};
        const VkPipelineCreateFlags2CreateInfo flags{.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
                                                     .flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT};
        const VkComputePipelineCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .pNext = b.heapMode ? &flags : nullptr,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      .pNext = &moduleInfo,
                      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                      .pName = "main"},
            .layout = b.heapMode ? VK_NULL_HANDLE : b.sets.pipelineLayout};
        VK(vkCreateComputePipelines(b.device, VK_NULL_HANDLE, 1, &info, nullptr, &b.pipelines[w]));
    }
}

void bindHeaps(Bench &b) {
    if (b.heapMode) {
        b.heap.bindResources(b.cmd, &b.heap.resourceBind);
        b.heap.bindSamplers(b.cmd, &b.heap.samplerBind);
    } else {
        vkCmdBindDescriptorSets(b.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, b.sets.pipelineLayout, 0, 2, b.sets.sets, 0,
                                nullptr);
    }
}

void setRoot(Bench &b, const Root &root) {
    if (b.heapMode) {
        const VkPushDataInfoEXT info{
            .sType = VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT, .offset = 0, .data = {&root, sizeof(root)}};
        b.heap.pushData(b.cmd, &info);
    } else {
        vkCmdPushConstants(b.cmd, b.sets.pipelineLayout, VK_SHADER_STAGE_ALL, 0, sizeof(root), &root);
    }
}

Root rootFor(const Bench &b) {
    return {.dst = b.outAddress,
            .count = kFetches,
            .slotMask = kHalf - 1,
            .textureScale = b.textureScale,
            .bufferScale = b.bufferScale,
            .bufferBase = kHalf,
            .samplerMask = kSamplers - 1};
}

// One dispatch of workload w between two timestamps; returns GPU milliseconds.
double runGpu(Bench &b, uint32_t w, bool keep) {
    begin(b);
    vkCmdResetQueryPool(b.cmd, b.queries, 0, 2);
    bindHeaps(b);
    vkCmdBindPipeline(b.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, b.pipelines[w]);
    const Root root = rootFor(b);
    setRoot(b, root);
    vkCmdWriteTimestamp2(b.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, b.queries, 0);
    vkCmdDispatch(b.cmd, kThreads / 64, 1, 1);
    vkCmdWriteTimestamp2(b.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, b.queries, 1);
    if (keep) {
        barrier(b.cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        const VkBufferCopy copy{0, 0, kThreads * 16ull};
        vkCmdCopyBuffer(b.cmd, b.out, b.readback, 1, &copy);
    }
    submitAndWait(b);
    uint64_t stamps[2];
    VK(vkGetQueryPoolResults(b.device, b.queries, 0, 2, sizeof(stamps), stamps, 8,
                             VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
    return double(stamps[1] - stamps[0]) * b.timestampPeriod * 1e-6;
}

// Records `count` pipeline binds, root data and dispatches; returns
// nanoseconds per iteration. Heaps are bound once, as RI does.
double runRecord(Bench &b, uint32_t count) {
    begin(b);
    bindHeaps(b);
    const Root root = rootFor(b);
    const double start = now();
    for (uint32_t i = 0; i < count; ++i) {
        vkCmdBindPipeline(b.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, b.pipelines[i & 1]);
        setRoot(b, root);
        vkCmdDispatch(b.cmd, 1, 1, 1);
    }
    const double seconds = now() - start;
    VK(vkEndCommandBuffer(b.cmd));
    return seconds * 1e9 / count;
}

} // namespace

int main(int argc, char **argv) {
    uint32_t gpuIndex = 0, rounds = 30;
    bool validate = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--gpu") && i + 1 < argc) {
            gpuIndex = uint32_t(std::atoi(argv[++i]));
        } else if (!std::strcmp(argv[i], "--rounds") && i + 1 < argc) {
            rounds = uint32_t(std::atoi(argv[++i]));
        } else if (!std::strcmp(argv[i], "--validate")) {
            validate = true;
        }
    }
    std::string dir = argv[0];
    dir = dir.find_last_of("/\\") == std::string::npos ? "." : dir.substr(0, dir.find_last_of("/\\"));

    const VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                .pApplicationName = "descriptor_bench",
                                .apiVersion = VK_API_VERSION_1_4};
    const char *layer = "VK_LAYER_KHRONOS_validation";
    const VkInstanceCreateInfo instanceInfo{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                            .pApplicationInfo = &app,
                                            .enabledLayerCount = validate ? 1u : 0u,
                                            .ppEnabledLayerNames = &layer};
    VK(volkInitialize());
    VkInstance instance = VK_NULL_HANDLE;
    VK(vkCreateInstance(&instanceInfo, nullptr, &instance));
    volkLoadInstance(instance); // device functions through the loader's trampolines: two devices
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> gpus(count);
    vkEnumeratePhysicalDevices(instance, &count, gpus.data());
    if (gpuIndex >= count) {
        std::fprintf(stderr, "no GPU %u\n", gpuIndex);
        return 1;
    }
    VkPhysicalDeviceDriverProperties driver{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver};
    vkGetPhysicalDeviceProperties2(gpus[gpuIndex], &properties);
    std::printf("gpu,%s\ndriver,%s %s\n", properties.properties.deviceName, driver.driverName, driver.driverInfo);
    std::printf("config,slots %u,textures %u,buffers %u,threads %u,fetches %u,rounds %u\n", kSlots, kTextures,
                kBuffers, kThreads, kFetches, rounds);

    Bench sets{.heapMode = false, .name = "sets"}, heap{.heapMode = true, .name = "heap"};
    for (Bench *b : {&sets, &heap}) {
        createDevice(*b, gpus[gpuIndex]);
        createResources(*b);
        if (b->heapMode) {
            createHeaps(*b);
        } else {
            createSets(*b);
        }
        createPipelines(*b, dir);
    }
    std::printf("heap,image descriptor %llu B,buffer descriptor %llu B,slot stride %llu B\n",
                (unsigned long long)heap.heap.properties.imageDescriptorSize,
                (unsigned long long)heap.heap.properties.bufferDescriptorSize,
                (unsigned long long)heap.heap.slotStride);

    // Descriptor writes (the first pass also fills the heaps for the GPU runs).
    std::vector<double> writeNs[2];
    for (uint32_t r = 0; r < 7; ++r) {
        for (Bench *b : {&sets, &heap}) {
            writeNs[b->heapMode].push_back(writeAll(*b) * 1e9 / kSlots);
        }
    }

    // Correctness: both mechanisms must compute the same sums.
    for (uint32_t w = 0; w < kWorkloadCount; ++w) {
        runGpu(sets, w, true);
        runGpu(heap, w, true);
        const float *a = static_cast<const float *>(sets.readbackMapped);
        const float *c = static_cast<const float *>(heap.readbackMapped);
        uint32_t mismatches = 0;
        for (uint32_t i = 0; i < kThreads * 4; ++i) {
            mismatches += std::abs(a[i] - c[i]) > 1e-3f * std::max(1.0f, std::abs(a[i]));
        }
        std::printf("check,%s,%s,first %.3f %.3f\n", kWorkloads[w], mismatches ? "MISMATCH" : "same", a[0], c[0]);
        if (mismatches) {
            std::printf("check,%s,%u mismatching values\n", kWorkloads[w], mismatches);
        }
    }

    // GPU time, interleaved: per round, each workload on each mechanism.
    std::vector<double> gpuMs[kWorkloadCount][2];
    for (uint32_t w = 0; w < kWorkloadCount; ++w) {
        runGpu(sets, w, false); // warm-up
        runGpu(heap, w, false);
    }
    for (uint32_t r = 0; r < rounds; ++r) {
        for (uint32_t w = 0; w < kWorkloadCount; ++w) {
            const bool heapFirst = (r + w) & 1;
            Bench &first = heapFirst ? heap : sets, &second = heapFirst ? sets : heap;
            gpuMs[w][first.heapMode].push_back(runGpu(first, w, false));
            gpuMs[w][second.heapMode].push_back(runGpu(second, w, false));
        }
    }

    std::vector<double> recordNs[2];
    for (uint32_t r = 0; r < 7; ++r) {
        for (Bench *b : {&sets, &heap}) {
            recordNs[b->heapMode].push_back(runRecord(*b, 20000));
        }
    }

    std::printf("measure,unit,sets,heap,heap/sets\n");
    for (uint32_t w = 0; w < kWorkloadCount; ++w) {
        const double s = median(gpuMs[w][0]), h = median(gpuMs[w][1]);
        std::printf("gpu %s,ms (median),%.4f,%.4f,%.3f\n", kWorkloads[w], s, h, h / s);
    }
    {
        const double s = median(writeNs[0]), h = median(writeNs[1]);
        std::printf("write descriptors,ns per descriptor (median),%.1f,%.1f,%.3f\n", s, h, h / s);
    }
    {
        const double s = median(recordNs[0]), h = median(recordNs[1]);
        std::printf("record pipeline+root+dispatch,ns (median),%.1f,%.1f,%.3f\n", s, h, h / s);
    }
    for (uint32_t w = 0; w < kWorkloadCount; ++w) {
        std::printf("raw gpu %s sets", kWorkloads[w]);
        for (double v : gpuMs[w][0]) {
            std::printf(",%.4f", v);
        }
        std::printf("\nraw gpu %s heap", kWorkloads[w]);
        for (double v : gpuMs[w][1]) {
            std::printf(",%.4f", v);
        }
        std::printf("\n");
    }
    for (Bench *b : {&sets, &heap}) {
        vkDeviceWaitIdle(b->device);
    }
    // The process exits here; the OS frees the devices (a benchmark, not RI).
    return 0;
}
