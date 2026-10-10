// Canonical Vulkan: buffer -> compute -> readback, the same program as
// snippets/ri/compute.cpp written with Vulkan alone (no RI). Bindless the way
// DXC compiles ResourceDescriptorHeap[]: one descriptor set whose binding 0 is
// a large array of mutable descriptors (VK_EXT_mutable_descriptor_type),
// updated after bind. Shader: snippets/hlsl/doc_compute.hlsl compiled with
//   dxc -T cs_6_6 -E main -spirv -fspv-target-env=vulkan1.3 -fvk-use-dx-layout
//       -fvk-bind-sampler-heap 0 1 -Fo doc_compute.spv doc_compute.hlsl
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

// --8<-- [start:find-memory-type]
// A memory type the resource accepts (`allowedTypes`, from its requirements)
// with every property in `required`.
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
// --8<-- [end:find-memory-type]

// --8<-- [start:allocate-function]
// One block of memory. The flags let buffers placed in it have a GPU address.
VkDeviceMemory allocate(VkDevice device, uint32_t type, VkDeviceSize size) {
    const VkMemoryAllocateFlagsInfo flags{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
    };
    const VkMemoryAllocateInfo info{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &flags,
        .allocationSize = size,
        .memoryTypeIndex = type,
    };
    VkDeviceMemory memory = VK_NULL_HANDLE;
    check(vkAllocateMemory(device, &info, nullptr, &memory), "vkAllocateMemory");
    return memory;
}
// --8<-- [end:allocate-function]

// --8<-- [start:create-buffer-function]
// A buffer placed in `memory` at `offset` (which must respect the alignment).
VkBuffer createBuffer(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, const VkBufferCreateInfo &info) {
    VkBuffer buffer = VK_NULL_HANDLE;
    check(vkCreateBuffer(device, &info, nullptr, &buffer), "vkCreateBuffer");
    check(vkBindBufferMemory(device, buffer, memory, offset), "vkBindBufferMemory");
    return buffer;
}
// --8<-- [end:create-buffer-function]

VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment) { return (value + alignment - 1) / alignment * alignment; }

// The C++ mirror of the shader's root data.
struct Root {
    uint32_t inputSlot;
    uint32_t outputSlot;
    uint32_t paramsSlot;
    float scale;
    uint32_t count;
};

} // namespace

int main(int argc, char **argv) {
    const char *shaderPath = argc > 1 ? argv[1] : "doc_compute.spv";

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
    // The features bindless compute needs, chained through pNext.
    VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mutableFeatures{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT,
        .mutableDescriptorType = VK_TRUE,
    };
    VkPhysicalDeviceVulkan13Features v13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                         .pNext = &mutableFeatures};
    v13.synchronization2 = VK_TRUE; // vkCmdPipelineBarrier2, vkQueueSubmit2
    VkPhysicalDeviceVulkan12Features v12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                         .pNext = &v13};
    v12.descriptorIndexing = VK_TRUE;
    v12.runtimeDescriptorArray = VK_TRUE;
    v12.descriptorBindingPartiallyBound = VK_TRUE;          // slots may stay empty
    v12.descriptorBindingVariableDescriptorCount = VK_TRUE; // the heap size is chosen per set
    v12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE; // write slots while the set is bound
    v12.descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
    v12.timelineSemaphore = VK_TRUE;
    v12.bufferDeviceAddress = VK_TRUE;

    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queueInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                            .queueFamilyIndex = queueFamily,
                                            .queueCount = 1,
                                            .pQueuePriorities = &priority};
    const char *const extensions[] = {VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME};
    const VkDeviceCreateInfo deviceInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                        .pNext = &v12,
                                        .queueCreateInfoCount = 1,
                                        .pQueueCreateInfos = &queueInfo,
                                        .enabledExtensionCount = 1,
                                        .ppEnabledExtensionNames = extensions};
    VkDevice device = VK_NULL_HANDLE;
    check(vkCreateDevice(gpu, &deviceInfo, nullptr, &device), "vkCreateDevice");
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, queueFamily, 0, &queue);
    // --8<-- [end:create-device]

    constexpr uint32_t kCount = 4096;
    constexpr VkDeviceSize kBytes = kCount * sizeof(float);

    // --8<-- [start:requirements]
    // Size, alignment and allowed memory types of a buffer, without creating it (Vulkan 1.3).
    const VkBufferCreateInfo dataInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = kBytes,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VkDeviceBufferMemoryRequirements query{.sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS,
                                                 .pCreateInfo = &dataInfo};
    VkMemoryRequirements2 dataReq{.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    vkGetDeviceBufferMemoryRequirements(device, &query, &dataReq);
    const VkMemoryRequirements &req = dataReq.memoryRequirements; // size, alignment, memoryTypeBits
    const VkDeviceSize outputOffset = alignUp(req.size, req.alignment);
    // --8<-- [end:requirements]

    const VkBufferCreateInfo stagingInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = kBytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    };
    const VkBufferCreateInfo paramsInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 256,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    };
    const VkBufferCreateInfo resultInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = kBytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    };
    const VkDeviceSize paramsOffset = alignUp(kBytes, 256);

    // --8<-- [start:allocate-memory]
    // GPU memory: device-local. Upload and readback: CPU-visible and coherent
    // (no flush/invalidate calls), mapped once for their whole lifetime.
    const VkMemoryPropertyFlags hostCoherent = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkDeviceMemory gpuMemory = allocate(
        device, findMemoryType(gpu, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), outputOffset + req.size);
    VkDeviceMemory uploadMemory =
        allocate(device, findMemoryType(gpu, req.memoryTypeBits, hostCoherent), paramsOffset + 256);
    VkDeviceMemory readbackMemory = allocate(
        device, findMemoryType(gpu, req.memoryTypeBits, hostCoherent | VK_MEMORY_PROPERTY_HOST_CACHED_BIT), kBytes);
    void *uploadMapped = nullptr;
    void *readbackMapped = nullptr;
    check(vkMapMemory(device, uploadMemory, 0, VK_WHOLE_SIZE, 0, &uploadMapped), "vkMapMemory");
    check(vkMapMemory(device, readbackMemory, 0, VK_WHOLE_SIZE, 0, &readbackMapped), "vkMapMemory");
    // --8<-- [end:allocate-memory]

    // --8<-- [start:create-buffers]
    VkBuffer input = createBuffer(device, gpuMemory, 0, dataInfo);
    VkBuffer output = createBuffer(device, gpuMemory, outputOffset, dataInfo);
    VkBuffer staging = createBuffer(device, uploadMemory, 0, stagingInfo);
    VkBuffer params = createBuffer(device, uploadMemory, paramsOffset, paramsInfo);
    VkBuffer result = createBuffer(device, readbackMemory, 0, resultInfo);
    // A buffer's GPU address (64-bit), usable in shaders and GPU-built tables.
    const VkBufferDeviceAddressInfo addressInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = input};
    const VkDeviceAddress inputAddress = vkGetBufferDeviceAddress(device, &addressInfo);
    // --8<-- [end:create-buffers]
    std::printf("input at GPU address 0x%llx\n", static_cast<unsigned long long>(inputAddress));

    // --8<-- [start:write-mapped]
    auto *values = static_cast<float *>(uploadMapped); // the staging buffer is at offset 0
    for (uint32_t i = 0; i < kCount; ++i) {
        values[i] = static_cast<float>(i);
    }
    const float offset = 0.25f;
    std::memcpy(static_cast<char *>(uploadMapped) + paramsOffset, &offset, sizeof(offset));
    // --8<-- [end:write-mapped]

    // --8<-- [start:set-layout]
    // The "heap": set 0, binding 0, an array of mutable descriptors (each slot
    // holds a storage or uniform buffer), of variable size, updatable after bind.
    const uint32_t kMaxSlots = 65536;
    const VkDescriptorType slotTypes[] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
    const VkMutableDescriptorTypeListEXT typeList{.descriptorTypeCount = 2, .pDescriptorTypes = slotTypes};
    const VkMutableDescriptorTypeCreateInfoEXT mutableInfo{
        .sType = VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT,
        .mutableDescriptorTypeListCount = 1,
        .pMutableDescriptorTypeLists = &typeList,
    };
    const VkDescriptorBindingFlags bindingFlags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                                  VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                                  VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
    const VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .pNext = &mutableInfo,
        .bindingCount = 1,
        .pBindingFlags = &bindingFlags,
    };
    const VkDescriptorSetLayoutBinding binding{.binding = 0,
                                               .descriptorType = VK_DESCRIPTOR_TYPE_MUTABLE_EXT,
                                               .descriptorCount = kMaxSlots,
                                               .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    const VkDescriptorSetLayoutCreateInfo layoutInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = &flagsInfo,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
        .bindingCount = 1,
        .pBindings = &binding,
    };
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    check(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &setLayout), "vkCreateDescriptorSetLayout");
    // --8<-- [end:set-layout]

    // --8<-- [start:create-heap]
    // A pool for one set, then the set itself with 16 slots.
    const uint32_t slots = 16;
    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_MUTABLE_EXT, slots};
    const VkDescriptorPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = &mutableInfo, // which types the pool's mutable descriptors may hold
        .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &poolSize,
    };
    VkDescriptorPool pool = VK_NULL_HANDLE;
    check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool), "vkCreateDescriptorPool");
    const VkDescriptorSetVariableDescriptorCountAllocateInfo countInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,
        .descriptorSetCount = 1,
        .pDescriptorCounts = &slots,
    };
    const VkDescriptorSetAllocateInfo setInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                              .pNext = &countInfo,
                                              .descriptorPool = pool,
                                              .descriptorSetCount = 1,
                                              .pSetLayouts = &setLayout};
    VkDescriptorSet heap = VK_NULL_HANDLE;
    check(vkAllocateDescriptorSets(device, &setInfo, &heap), "vkAllocateDescriptorSets");
    // --8<-- [end:create-heap]

    // --8<-- [start:write-descriptors]
    // Slot = array element. Read and read-write views are both storage
    // buffers in SPIR-V; the constant buffer is a uniform buffer.
    const VkDescriptorBufferInfo inputView{.buffer = input, .offset = 0, .range = kBytes};
    const VkDescriptorBufferInfo outputView{.buffer = output, .offset = 0, .range = kBytes};
    const VkDescriptorBufferInfo paramsView{.buffer = params, .offset = 0, .range = 256};
    const auto write = [&](uint32_t slot, VkDescriptorType type, const VkDescriptorBufferInfo *view) {
        return VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                    .dstSet = heap,
                                    .dstBinding = 0,
                                    .dstArrayElement = slot,
                                    .descriptorCount = 1,
                                    .descriptorType = type,
                                    .pBufferInfo = view};
    };
    const VkWriteDescriptorSet writes[] = {
        write(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &inputView),
        write(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &outputView),
        write(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &paramsView),
    };
    vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
    // --8<-- [end:write-descriptors]

    // --8<-- [start:create-pipeline]
    // The layout: the heap set, plus push constants of exactly the root data's size.
    const VkPushConstantRange pushRange{.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(Root)};
    const VkPipelineLayoutCreateInfo pipelineLayoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                        .setLayoutCount = 1,
                                                        .pSetLayouts = &setLayout,
                                                        .pushConstantRangeCount = 1,
                                                        .pPushConstantRanges = &pushRange};
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    check(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout), "vkCreatePipelineLayout");

    const std::vector<uint32_t> spirv = loadSpirv(shaderPath);
    const VkShaderModuleCreateInfo moduleInfo{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                              .codeSize = spirv.size() * 4,
                                              .pCode = spirv.data()};
    VkShaderModule module = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device, &moduleInfo, nullptr, &module), "vkCreateShaderModule");
    const VkComputePipelineCreateInfo pipelineInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = module,
                  .pName = "main"},
        .layout = pipelineLayout,
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline),
          "vkCreateComputePipelines");
    vkDestroyShaderModule(device, module, nullptr); // not needed once the pipeline exists
    // --8<-- [end:create-pipeline]

    // --8<-- [start:create-command-list]
    // A pool per command buffer, so resetting the pool resets just this list.
    const VkCommandPoolCreateInfo commandPoolInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                                  .queueFamilyIndex = queueFamily};
    VkCommandPool commandPool = VK_NULL_HANDLE;
    check(vkCreateCommandPool(device, &commandPoolInfo, nullptr, &commandPool), "vkCreateCommandPool");
    const VkCommandBufferAllocateInfo commandInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                  .commandPool = commandPool,
                                                  .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                  .commandBufferCount = 1};
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(device, &commandInfo, &cmd), "vkAllocateCommandBuffers");
    // --8<-- [end:create-command-list]

    // --8<-- [start:barrier-function]
    // A global memory barrier: every write of `srcStages` becomes visible to
    // every access of `dstStages`. Buffers have no layouts, so this is all they need.
    const auto barrier = [&](VkPipelineStageFlags2 srcStages, VkPipelineStageFlags2 dstStages,
                             VkAccessFlags2 dstAccess) {
        const VkMemoryBarrier2 memoryBarrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                             .srcStageMask = srcStages,
                                             .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                                             .dstStageMask = dstStages,
                                             .dstAccessMask = dstAccess};
        const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                          .memoryBarrierCount = 1,
                                          .pMemoryBarriers = &memoryBarrier};
        vkCmdPipelineBarrier2(cmd, &dependency);
    };
    // --8<-- [end:barrier-function]

    // --8<-- [start:record]
    // --8<-- [start:cmd-begin]
    const VkCommandBufferBeginInfo beginInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                             .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    check(vkBeginCommandBuffer(cmd, &beginInfo), "vkBeginCommandBuffer");
    // --8<-- [end:cmd-begin]
    // --8<-- [start:copy-in]
    const VkBufferCopy copyIn{.srcOffset = 0, .dstOffset = 0, .size = kBytes};
    vkCmdCopyBuffer(cmd, staging, input, 1, &copyIn);
    // --8<-- [end:copy-in]
    // --8<-- [start:barrier-copy-compute]
    barrier(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    // --8<-- [end:barrier-copy-compute]
    // --8<-- [start:set-heap]
    // The same order as RI's rule: the heap first, then the pipeline.
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &heap, 0, nullptr);
    // --8<-- [end:set-heap]
    // --8<-- [start:set-pipeline]
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    // --8<-- [end:set-pipeline]
    // --8<-- [start:root-data]
    const Root root{.inputSlot = 0, .outputSlot = 1, .paramsSlot = 2, .scale = 3.0f, .count = kCount};
    vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(root), &root);
    // --8<-- [end:root-data]
    // --8<-- [start:dispatch]
    vkCmdDispatch(cmd, (kCount + 63) / 64, 1, 1);
    // --8<-- [end:dispatch]
    // --8<-- [start:barrier-compute-copy]
    barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    // --8<-- [end:barrier-compute-copy]
    // --8<-- [start:copy-out]
    const VkBufferCopy copyOut{.srcOffset = 0, .dstOffset = 0, .size = kBytes};
    vkCmdCopyBuffer(cmd, output, result, 1, &copyOut);
    // --8<-- [end:copy-out]
    // --8<-- [start:cmd-end]
    // Make the copy's writes visible to the CPU (the host stage) before ending.
    barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
    // --8<-- [end:cmd-end]
    // --8<-- [end:record]

    // --8<-- [start:timeline]
    // A timeline semaphore: a 64-bit counter the GPU raises as work completes.
    const VkSemaphoreTypeCreateInfo timelineType{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                                 .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
                                                 .initialValue = 0};
    const VkSemaphoreCreateInfo semaphoreInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &timelineType};
    VkSemaphore timeline = VK_NULL_HANDLE;
    check(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &timeline), "vkCreateSemaphore");
    uint64_t lastSignaled = 0;
    // --8<-- [end:timeline]

    // --8<-- [start:submit]
    // Submit, asking the GPU to raise the counter to the next value when done.
    const uint64_t done = ++lastSignaled;
    const VkCommandBufferSubmitInfo commandSubmit{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                                  .commandBuffer = cmd};
    const VkSemaphoreSubmitInfo signal{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                       .semaphore = timeline,
                                       .value = done,
                                       .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    const VkSubmitInfo2 submitInfo{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                                   .commandBufferInfoCount = 1,
                                   .pCommandBufferInfos = &commandSubmit,
                                   .signalSemaphoreInfoCount = 1,
                                   .pSignalSemaphoreInfos = &signal};
    check(vkQueueSubmit2(queue, 1, &submitInfo, VK_NULL_HANDLE), "vkQueueSubmit2");
    // --8<-- [end:submit]

    // --8<-- [start:wait]
    const VkSemaphoreWaitInfo waitInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                       .semaphoreCount = 1,
                                       .pSemaphores = &timeline,
                                       .pValues = &done};
    check(vkWaitSemaphores(device, &waitInfo, UINT64_MAX), "vkWaitSemaphores");
    // --8<-- [end:wait]

    // --8<-- [start:is-reached]
    uint64_t reached = 0;
    check(vkGetSemaphoreCounterValue(device, timeline, &reached), "vkGetSemaphoreCounterValue");
    const bool finished = reached >= done; // never blocks
    // --8<-- [end:is-reached]

    // --8<-- [start:read-back]
    const auto *out = static_cast<const float *>(readbackMapped);
    std::printf("out[1] = %g (expected 3.25), finished: %d\n", out[1], finished ? 1 : 0);
    // --8<-- [end:read-back]

    // --8<-- [start:reset]
    // Only after the GPU is done with it: resets every command buffer of the pool.
    check(vkResetCommandPool(device, commandPool, 0), "vkResetCommandPool");
    // --8<-- [end:reset]

    // --8<-- [start:destroy]
    vkDestroySemaphore(device, timeline, nullptr);
    vkDestroyCommandPool(device, commandPool, nullptr); // frees its command buffers
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    vkDestroyDescriptorPool(device, pool, nullptr); // frees its sets
    vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    for (VkBuffer buffer : {input, output, staging, params, result}) {
        vkDestroyBuffer(device, buffer, nullptr);
    }
    for (VkDeviceMemory memory : {gpuMemory, uploadMemory, readbackMemory}) {
        vkFreeMemory(device, memory, nullptr); // unmaps it too
    }
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    // --8<-- [end:destroy]
    return 0;
}
