// The P112 measurement: what a bindful constant buffer costs on Vulkan when it
// is a push descriptor, against a heap-indexed constant buffer (an index in
// root data into an array of uniform buffers, bound once) and against root
// data alone (the block in the push constants).
//
// Raw Vulkan, no RI: one device, the same 64-byte blocks for every mode, and
// rounds that alternate the modes so clock changes hit them alike. Measured:
//   - many: kDispatches one-group dispatches, each with its own block (the
//     per-pass or per-draw case): CPU time to record them and GPU time to run
//     them;
//   - heavy: one large dispatch whose threads read the block kReads times
//     (the read path of a block that does not change).
//
// Usage: constant_bench [--gpu N] [--rounds N]

#include <volk.h>

#include <algorithm>
#include <chrono>
#include <cmath>
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

constexpr uint32_t kDispatches = 4096; // "many": one block per dispatch
constexpr uint32_t kHeavyGroups = 16384;
constexpr uint32_t kHeavyReads = 256;
constexpr uint32_t kManyReads = 4;
constexpr uint32_t kBlockStride = 256; // D3D12's constant-buffer alignment, kept on Vulkan (P112)
const char *const kModes[] = {"push", "heap", "root"};
constexpr uint32_t kModeCount = 3;

struct Block {
    float v[16];
};

struct Root {
    VkDeviceAddress out;
    uint32_t index;
    uint32_t reads;
    Block block; // ROOT only; the other modes push the first 16 bytes
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

Block blockOf(uint32_t i) {
    Block b{};
    for (uint32_t k = 0; k < 16; ++k) {
        b.v[k] = float((i * 16 + k) % 97) * 0.25f;
    }
    return b;
}

struct Bench {
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    bool v14 = false;
    VkPhysicalDeviceMemoryProperties memory{};
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;
    float timestampPeriod = 1.0f;
    VkBuffer blocks = VK_NULL_HANDLE, out = VK_NULL_HANDLE, readback = VK_NULL_HANDLE;
    VkDeviceMemory blocksMemory = VK_NULL_HANDLE, outMemory = VK_NULL_HANDLE, readbackMemory = VK_NULL_HANDLE;
    VkDeviceAddress outAddress = 0;
    void *blocksMapped = nullptr, *readbackMapped = nullptr;
    VkDescriptorSetLayout setLayouts[kModeCount]{};
    VkPipelineLayout layouts[kModeCount]{};
    VkPipeline pipelines[kModeCount]{};
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet heapSet = VK_NULL_HANDLE;
};

uint32_t memoryType(const Bench &b, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < b.memory.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags flags = b.memory.memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) && (flags & want) == want && !(flags & VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD)) {
            return i;
        }
    }
    std::fprintf(stderr, "no memory type\n");
    std::exit(1);
}

VkBuffer createBuffer(const Bench &b, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want,
                      VkDeviceMemory &memory) {
    const VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    VkBuffer buffer = VK_NULL_HANDLE;
    VK(vkCreateBuffer(b.device, &info, nullptr, &buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(b.device, buffer, &req);
    const VkMemoryAllocateFlagsInfo flags{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                          .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    const VkMemoryAllocateInfo allocation{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                          .pNext = &flags,
                                          .allocationSize = req.size,
                                          .memoryTypeIndex = memoryType(b, req.memoryTypeBits, want)};
    VK(vkAllocateMemory(b.device, &allocation, nullptr, &memory));
    VK(vkBindBufferMemory(b.device, buffer, memory, 0));
    return buffer;
}

void createDevice(Bench &b, VkPhysicalDevice gpu) {
    b.gpu = gpu;
    vkGetPhysicalDeviceMemoryProperties(gpu, &b.memory);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(gpu, &properties);
    b.timestampPeriod = properties.limits.timestampPeriod;
    b.v14 = properties.apiVersion >= VK_API_VERSION_1_4;
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
    VkPhysicalDeviceVulkan14Features v14{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
                                         .pushDescriptor = VK_TRUE};
    VkPhysicalDeviceVulkan13Features v13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                         .pNext = b.v14 ? &v14 : nullptr,
                                         .synchronization2 = VK_TRUE};
    VkPhysicalDeviceVulkan12Features v12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                         .pNext = &v13,
                                         .descriptorIndexing = VK_TRUE,
                                         .descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE,
                                         .descriptorBindingPartiallyBound = VK_TRUE,
                                         .runtimeDescriptorArray = VK_TRUE,
                                         .bufferDeviceAddress = VK_TRUE};
    VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v12};
    features.features.shaderInt64 = VK_TRUE;
    std::vector<const char *> extensions;
    if (!b.v14) {
        extensions.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    }
    const VkDeviceCreateInfo info{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                  .pNext = &features,
                                  .queueCreateInfoCount = 1,
                                  .pQueueCreateInfos = &queue,
                                  .enabledExtensionCount = uint32_t(extensions.size()),
                                  .ppEnabledExtensionNames = extensions.data()};
    VK(vkCreateDevice(gpu, &info, nullptr, &b.device));
    volkLoadDevice(b.device);
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

void createResources(Bench &b) {
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    // Blocks live in host-visible memory, as a per-frame upload ring would put them.
    b.blocks = createBuffer(b, VkDeviceSize(kDispatches) * kBlockStride, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, host,
                            b.blocksMemory);
    VK(vkMapMemory(b.device, b.blocksMemory, 0, VK_WHOLE_SIZE, 0, &b.blocksMapped));
    for (uint32_t i = 0; i < kDispatches; ++i) {
        const Block block = blockOf(i);
        std::memcpy(static_cast<std::byte *>(b.blocksMapped) + size_t(i) * kBlockStride, &block, sizeof(block));
    }
    const VkDeviceSize outSize = VkDeviceSize(std::max(kDispatches, kHeavyGroups)) * 64 * 16;
    b.out = createBuffer(b, outSize,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, b.outMemory);
    const VkBufferDeviceAddressInfo address{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = b.out};
    b.outAddress = vkGetBufferDeviceAddress(b.device, &address);
    b.readback = createBuffer(b, outSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, host, b.readbackMemory);
    VK(vkMapMemory(b.device, b.readbackMemory, 0, VK_WHOLE_SIZE, 0, &b.readbackMapped));
}

void createPipelines(Bench &b, const std::string &dir) {
    const VkDescriptorSetLayoutBinding pushBinding{.binding = 0,
                                                   .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                   .descriptorCount = 1,
                                                   .stageFlags = VK_SHADER_STAGE_ALL};
    const VkDescriptorSetLayoutCreateInfo pushInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                   .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT,
                                                   .bindingCount = 1,
                                                   .pBindings = &pushBinding};
    VK(vkCreateDescriptorSetLayout(b.device, &pushInfo, nullptr, &b.setLayouts[0]));

    const VkDescriptorSetLayoutBinding heapBinding{.binding = 0,
                                                   .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                   .descriptorCount = kDispatches,
                                                   .stageFlags = VK_SHADER_STAGE_ALL};
    const VkDescriptorBindingFlags heapFlags =
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    const VkDescriptorSetLayoutBindingFlagsCreateInfo heapFlagsInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = 1,
        .pBindingFlags = &heapFlags};
    const VkDescriptorSetLayoutCreateInfo heapInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                   .pNext = &heapFlagsInfo,
                                                   .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
                                                   .bindingCount = 1,
                                                   .pBindings = &heapBinding};
    VK(vkCreateDescriptorSetLayout(b.device, &heapInfo, nullptr, &b.setLayouts[1]));

    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kDispatches};
    const VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                              .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
                                              .maxSets = 1,
                                              .poolSizeCount = 1,
                                              .pPoolSizes = &poolSize};
    VK(vkCreateDescriptorPool(b.device, &poolInfo, nullptr, &b.descriptorPool));
    const VkDescriptorSetAllocateInfo allocation{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                 .descriptorPool = b.descriptorPool,
                                                 .descriptorSetCount = 1,
                                                 .pSetLayouts = &b.setLayouts[1]};
    VK(vkAllocateDescriptorSets(b.device, &allocation, &b.heapSet));
    std::vector<VkDescriptorBufferInfo> infos(kDispatches);
    for (uint32_t i = 0; i < kDispatches; ++i) {
        infos[i] = {b.blocks, VkDeviceSize(i) * kBlockStride, sizeof(Block)};
    }
    const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                     .dstSet = b.heapSet,
                                     .descriptorCount = kDispatches,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                     .pBufferInfo = infos.data()};
    vkUpdateDescriptorSets(b.device, 1, &write, 0, nullptr);

    for (uint32_t m = 0; m < kModeCount; ++m) {
        const VkPushConstantRange range{.stageFlags = VK_SHADER_STAGE_ALL,
                                        .offset = 0,
                                        .size = m == 2 ? uint32_t(sizeof(Root)) : uint32_t(offsetof(Root, block))};
        const VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                    .setLayoutCount = m == 2 ? 0u : 1u,
                                                    .pSetLayouts = &b.setLayouts[m],
                                                    .pushConstantRangeCount = 1,
                                                    .pPushConstantRanges = &range};
        VK(vkCreatePipelineLayout(b.device, &layoutInfo, nullptr, &b.layouts[m]));
        const std::vector<uint32_t> code = readSpirv(dir + "/constant_" + kModes[m] + ".spv");
        const VkShaderModuleCreateInfo moduleInfo{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                                  .codeSize = code.size() * 4,
                                                  .pCode = code.data()};
        VkShaderModule module = VK_NULL_HANDLE;
        VK(vkCreateShaderModule(b.device, &moduleInfo, nullptr, &module));
        const VkComputePipelineCreateInfo pipelineInfo{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                      .module = module,
                      .pName = "main"},
            .layout = b.layouts[m]};
        VK(vkCreateComputePipelines(b.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &b.pipelines[m]));
        vkDestroyShaderModule(b.device, module, nullptr);
    }
}

void pushBlock(Bench &b, uint32_t index) {
    const VkDescriptorBufferInfo info{b.blocks, VkDeviceSize(index) * kBlockStride, sizeof(Block)};
    const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                     .dstBinding = 0,
                                     .descriptorCount = 1,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                     .pBufferInfo = &info};
    if (b.v14) {
        vkCmdPushDescriptorSet(b.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, b.layouts[0], 0, 1, &write);
    } else {
        vkCmdPushDescriptorSetKHR(b.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, b.layouts[0], 0, 1, &write);
    }
}

// Records one dispatch of `groups` groups reading block `index`, the way each mode binds it.
void recordDispatch(Bench &b, uint32_t m, uint32_t index, uint32_t reads, uint32_t groups) {
    Root root{.out = b.outAddress, .index = index, .reads = reads, .block = {}};
    uint32_t rootSize = offsetof(Root, block);
    if (m == 0) {
        pushBlock(b, index);
    } else if (m == 2) {
        root.block = blockOf(index);
        rootSize = sizeof(Root);
    }
    vkCmdPushConstants(b.cmd, b.layouts[m], VK_SHADER_STAGE_ALL, 0, rootSize, &root);
    vkCmdDispatch(b.cmd, groups, 1, 1);
}

void begin(Bench &b) {
    VK(vkResetCommandPool(b.device, b.pool, 0));
    const VkCommandBufferBeginInfo info{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK(vkBeginCommandBuffer(b.cmd, &info));
    vkCmdResetQueryPool(b.cmd, b.queries, 0, 2);
}

void bindMode(Bench &b, uint32_t m) {
    vkCmdBindPipeline(b.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, b.pipelines[m]);
    if (m == 1) {
        vkCmdBindDescriptorSets(b.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, b.layouts[1], 0, 1, &b.heapSet, 0, nullptr);
    }
}

// Submits, waits, and returns the GPU time between the two timestamps in ms.
double submitAndTime(Bench &b, bool copyOut) {
    if (copyOut) {
        const VkMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                       .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                       .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
                                       .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                       .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
        vkCmdPipelineBarrier2(b.cmd, &dependency);
        const VkBufferCopy copy{0, 0, VkDeviceSize(std::max(kDispatches, kHeavyGroups)) * 64 * 16};
        vkCmdCopyBuffer(b.cmd, b.out, b.readback, 1, &copy);
    }
    VK(vkEndCommandBuffer(b.cmd));
    const VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &b.cmd};
    VK(vkResetFences(b.device, 1, &b.fence));
    VK(vkQueueSubmit(b.queue, 1, &submit, b.fence));
    VK(vkWaitForFences(b.device, 1, &b.fence, VK_TRUE, UINT64_MAX));
    uint64_t stamps[2] = {};
    VK(vkGetQueryPoolResults(b.device, b.queries, 0, 2, sizeof(stamps), stamps, 8,
                             VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
    return double(stamps[1] - stamps[0]) * b.timestampPeriod * 1e-6;
}

struct Many {
    double cpuNsPerDispatch;
    double gpuMs;
};

Many runMany(Bench &b, uint32_t m, bool copyOut) {
    begin(b);
    vkCmdWriteTimestamp2(b.cmd, VK_PIPELINE_STAGE_2_NONE, b.queries, 0);
    const double start = now();
    bindMode(b, m);
    for (uint32_t i = 0; i < kDispatches; ++i) {
        recordDispatch(b, m, i, kManyReads, 1);
    }
    const double recorded = now() - start;
    vkCmdWriteTimestamp2(b.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, b.queries, 1);
    return {recorded * 1e9 / kDispatches, submitAndTime(b, copyOut)};
}

double runHeavy(Bench &b, uint32_t m, bool copyOut) {
    begin(b);
    bindMode(b, m);
    vkCmdWriteTimestamp2(b.cmd, VK_PIPELINE_STAGE_2_NONE, b.queries, 0);
    recordDispatch(b, m, 0, kHeavyReads, kHeavyGroups);
    vkCmdWriteTimestamp2(b.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, b.queries, 1);
    return submitAndTime(b, copyOut);
}

std::vector<float> readOut(const Bench &b, size_t count) {
    std::vector<float> values(count);
    std::memcpy(values.data(), b.readbackMapped, count * sizeof(float));
    return values;
}

} // namespace

int main(int argc, char **argv) {
    uint32_t gpuIndex = 0;
    uint32_t rounds = 15;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--gpu" && i + 1 < argc) {
            gpuIndex = uint32_t(std::atoi(argv[++i]));
        } else if (arg == "--rounds" && i + 1 < argc) {
            rounds = uint32_t(std::atoi(argv[++i]));
        } else {
            std::fprintf(stderr, "usage: constant_bench [--gpu N] [--rounds N]\n");
            return 1;
        }
    }
    VK(volkInitialize());
    const VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_4};
    const VkInstanceCreateInfo instanceInfo{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance = VK_NULL_HANDLE;
    VK(vkCreateInstance(&instanceInfo, nullptr, &instance));
    volkLoadInstance(instance);
    uint32_t gpuCount = 0;
    vkEnumeratePhysicalDevices(instance, &gpuCount, nullptr);
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    vkEnumeratePhysicalDevices(instance, &gpuCount, gpus.data());
    if (gpuIndex >= gpuCount) {
        std::fprintf(stderr, "no GPU %u\n", gpuIndex);
        return 1;
    }
    VkPhysicalDeviceDriverProperties driver{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver};
    vkGetPhysicalDeviceProperties2(gpus[gpuIndex], &properties);
    std::printf("gpu,%s\ndriver,%s %s\n", properties.properties.deviceName, driver.driverName, driver.driverInfo);
    std::printf("config,dispatches %u,many reads %u,heavy groups %u,heavy reads %u,rounds %u\n", kDispatches, kManyReads,
                kHeavyGroups, kHeavyReads, rounds);

    Bench b;
    createDevice(b, gpus[gpuIndex]);
    createResources(b);
    std::string dir = argv[0];
    dir = dir.substr(0, dir.find_last_of("/\\"));
    createPipelines(b, dir);

    // Every mode must compute the same values from the same blocks.
    std::vector<float> reference;
    for (uint32_t m = 0; m < kModeCount; ++m) {
        runMany(b, m, true);
        const std::vector<float> many = readOut(b, size_t(kDispatches) * 64 * 4);
        runHeavy(b, m, true);
        std::vector<float> values = readOut(b, size_t(kHeavyGroups) * 64 * 4);
        values.insert(values.end(), many.begin(), many.end());
        if (m == 0) {
            reference = values;
            continue;
        }
        uint32_t mismatches = 0;
        for (size_t i = 0; i < values.size(); ++i) {
            if (std::fabs(values[i] - reference[i]) > 1e-3f * std::max(1.0f, std::fabs(reference[i]))) {
                ++mismatches;
            }
        }
        std::printf("check,%s,%s\n", kModes[m], mismatches == 0 ? "same as push" : "MISMATCH");
        if (mismatches != 0) {
            return 1;
        }
    }

    std::vector<double> manyCpu[kModeCount], manyGpu[kModeCount], heavyGpu[kModeCount];
    for (uint32_t r = 0; r < rounds; ++r) {
        for (uint32_t k = 0; k < kModeCount; ++k) {
            const uint32_t m = (r + k) % kModeCount;
            const Many many = runMany(b, m, false);
            manyCpu[m].push_back(many.cpuNsPerDispatch);
            manyGpu[m].push_back(many.gpuMs);
            heavyGpu[m].push_back(runHeavy(b, m, false));
        }
    }
    std::printf("measure,unit,push,heap,root\n");
    std::printf("many: record,ns per dispatch (median),%.1f,%.1f,%.1f\n", median(manyCpu[0]), median(manyCpu[1]),
                median(manyCpu[2]));
    std::printf("many: gpu,ms for %u dispatches (median),%.4f,%.4f,%.4f\n", kDispatches, median(manyGpu[0]),
                median(manyGpu[1]), median(manyGpu[2]));
    std::printf("heavy: gpu,ms (median),%.4f,%.4f,%.4f\n", median(heavyGpu[0]), median(heavyGpu[1]),
                median(heavyGpu[2]));
    vkDeviceWaitIdle(b.device);
    return 0;
}
