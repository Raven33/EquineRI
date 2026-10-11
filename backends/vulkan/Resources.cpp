// Memory, buffers and descriptor heaps (P31, P32, P33, P58, P59, P61, P87, P94).

#include "DeviceState.hpp"
#include "VkCheck.hpp"

#include <RI/Error.hpp>
#include <RI/Validation/Checks.hpp>
#include <RI/Vk/Native.hpp>

#include <algorithm>
#include <format>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

namespace RI::Vk {

namespace {

const char *nameOf(MemoryKind kind) {
    switch (kind) {
    case MemoryKind::eGPU: return "eGPU";
    case MemoryKind::eUpload: return "eUpload";
    case MemoryKind::eReadback: return "eReadback";
    case MemoryKind::eGPUUpload: return "eGPUUpload";
    }
    return "?";
}

VkBufferUsageFlags usageOf(BufferUsage usage) {
    // Every buffer has a GPU address (P61).
    VkBufferUsageFlags flags = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if ((usage & (BufferUsage::eRead | BufferUsage::eReadWrite)) != BufferUsage::eNone) {
        flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    if ((usage & BufferUsage::eConstant) != BufferUsage::eNone) {
        flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    }
    if ((usage & BufferUsage::eCopySrc) != BufferUsage::eNone) {
        flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    }
    if ((usage & BufferUsage::eCopyDst) != BufferUsage::eNone) {
        flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }
    if ((usage & BufferUsage::eIndirect) != BufferUsage::eNone) {
        flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    }
    if ((usage & BufferUsage::eIndex) != BufferUsage::eNone) {
        flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    }
    return flags;
}

VkBufferCreateInfo bufferInfo(const BufferDesc &desc) {
    return {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = desc.size,
        .usage = usageOf(desc.usage),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
}

} // namespace

// Every memory kind whose native type is in `typeBits` (P58 (1)).
uint32 kindsOf(const DeviceState &state, uint32 typeBits) {
    uint32 kinds = 0;
    for (uint32 kind = 0; kind < state.memoryTypes.size(); ++kind) {
        const uint32 type = state.memoryTypes[kind];
        if (type != UINT32_MAX && (typeBits & (1u << type)) != 0) {
            kinds |= 1u << kind;
        }
    }
    return kinds;
}

// ── Memory ───────────────────────────────────────────────────────────────────

Memory allocateMemory(Device &device, MemoryKind kind, uint64 size) {
    DeviceState &state = *device.internal.state;
    const uint32 type = state.memoryTypes[kindIndex(kind)];
    if (type == UINT32_MAX) {
        reportError(ErrorCategory::eUnsupported,
                    std::format("memory kind {} is not available on '{}' (see DeviceLimits::gpuUploadMemory)",
                                nameOf(kind), device.gpu.name));
    }
    const VkMemoryAllocateFlagsInfo flags{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    const VkMemoryAllocateInfo info{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &flags, .allocationSize = size, .memoryTypeIndex = type};
    VkDeviceMemory memory = VK_NULL_HANDLE;
    checkVk(state.functions.vkAllocateMemory(state.device, &info, nullptr, &memory), "vkAllocateMemory");

    Memory result;
    result.kind = kind;
    result.size = size;
    // CPU-visible kinds stay mapped for their lifetime (P33).
    if (state.memoryKindMapped[kindIndex(kind)]) {
        checkVk(state.functions.vkMapMemory(state.device, memory, 0, VK_WHOLE_SIZE, 0, &result.mapped), "vkMapMemory");
    }
    result.internal.memory = reinterpret_cast<uint64>(memory);
    result.internal.memoryType = type;
    result.internal.id = EQ_VK_TRACK(&state);
    return result;
}

void freeMemory(Device &device, Memory &memory) {
    DeviceState &state = *device.internal.state;
    EQ_VK_CHECK_ALIVE(&state, memory.internal.id, "memory");
    EQ_VK_UNTRACK(&state, memory.internal.id);
#if EQUINE_VALIDATION
    Validation::forgetMemory(state.placements, memory.internal.id);
#endif
    if (!memory.internal.imported) { // P77: imported memory stays the user's
        state.functions.vkFreeMemory(state.device, native(memory), nullptr);
    }
    memory = {};
}

// ── Buffers ──────────────────────────────────────────────────────────────────

MemoryRequirements bufferRequirements(Device &device, const BufferDesc &desc) {
    DeviceState &state = *device.internal.state;
    const VkBufferCreateInfo createInfo = bufferInfo(desc);
    const VkDeviceBufferMemoryRequirements info{.sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS,
                                                .pCreateInfo = &createInfo};
    VkMemoryRequirements2 requirements{.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    state.functions.vkGetDeviceBufferMemoryRequirements(state.device, &info, &requirements);
    return withGranularity(state, {.size = requirements.memoryRequirements.size,
                                   .alignment = requirements.memoryRequirements.alignment,
                                   .kinds = kindsOf(state, requirements.memoryRequirements.memoryTypeBits)});
}

Buffer createBuffer(Device &device, const Memory &memory, uint64 offset, const BufferDesc &desc) {
    DeviceState &state = *device.internal.state;
    EQ_VK_CHECK_ALIVE(&state, memory.internal.id, "memory");
    [[maybe_unused]] const MemoryRequirements requirements = bufferRequirements(device, desc);
    EQ_VALIDATE(Validation::checkRange(offset, requirements.size, memory.size, requirements.alignment,
                                       "createBuffer: placing the buffer in memory"));

    const VkBufferCreateInfo info = bufferInfo(desc);
    VkBuffer buffer = VK_NULL_HANDLE;
    checkVk(state.functions.vkCreateBuffer(state.device, &info, nullptr, &buffer), "vkCreateBuffer");
    checkVk(state.functions.vkBindBufferMemory(state.device, buffer, native(memory), offset), "vkBindBufferMemory");

    Buffer result;
    result.size = desc.size;
    result.usage = desc.usage;
    const VkBufferDeviceAddressInfo addressInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffer};
    result.gpuAddress = state.functions.vkGetBufferDeviceAddress(state.device, &addressInfo);
    result.mapped = memory.mapped != nullptr ? static_cast<std::byte *>(memory.mapped) + offset : nullptr;
    result.internal.buffer = reinterpret_cast<uint64>(buffer);
    result.internal.id = EQ_VK_TRACK(&state);
#if EQUINE_VALIDATION
    Validation::placeResource(state.placements, memory.internal.id, offset, requirements.size, result.internal.id,
                              false);
#endif
    return result;
}

void destroyBuffer(Device &device, Buffer &buffer) {
    DeviceState &state = *device.internal.state;
    EQ_VK_CHECK_ALIVE(&state, buffer.internal.id, "buffer");
    EQ_VK_UNTRACK(&state, buffer.internal.id);
    if (!buffer.internal.imported) { // P77: an imported buffer stays the user's
        state.functions.vkDestroyBuffer(state.device, native(buffer), nullptr);
    }
    buffer = {};
}

// ── Descriptor heaps (P94) ───────────────────────────────────────────────────

DescriptorHeap createDescriptorHeap(Device &device, const DescriptorHeapDesc &desc) {
    DeviceState &state = *device.internal.state;
    if (desc.resourceCount > device.limits.maxResourceDescriptors ||
        desc.samplerCount > device.limits.maxSamplerDescriptors) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("createDescriptorHeap: {} resource / {} sampler slots exceed the device limits "
                                "({} / {}, Device::limits)",
                                desc.resourceCount, desc.samplerCount, device.limits.maxResourceDescriptors,
                                device.limits.maxSamplerDescriptors));
    }

    const VkDescriptorType mutableTypes[] = {
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    };
    const VkMutableDescriptorTypeListEXT lists[] = {{.descriptorTypeCount = 6, .pDescriptorTypes = mutableTypes},
                                                    {}};
    const VkMutableDescriptorTypeCreateInfoEXT mutableInfo{
        .sType = VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT,
        .mutableDescriptorTypeListCount = 2,
        .pMutableDescriptorTypeLists = lists,
    };
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_MUTABLE_EXT, std::max(desc.resourceCount, 1u)},
        {VK_DESCRIPTOR_TYPE_SAMPLER, std::max(desc.samplerCount, 1u)},
    };
    const VkDescriptorPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = &mutableInfo,
        .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
        .maxSets = 2,
        .poolSizeCount = 2,
        .pPoolSizes = sizes,
    };
    VkDescriptorPool pool = VK_NULL_HANDLE;
    checkVk(state.functions.vkCreateDescriptorPool(state.device, &poolInfo, nullptr, &pool), "vkCreateDescriptorPool");

    // At least one slot each: RADV fails to allocate a variable-count set of 0.
    const uint32 counts[] = {std::max(desc.resourceCount, 1u), std::max(desc.samplerCount, 1u)};
    const VkDescriptorSetVariableDescriptorCountAllocateInfo countInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,
        .descriptorSetCount = 2,
        .pDescriptorCounts = counts,
    };
    const VkDescriptorSetLayout layouts[] = {state.resourceSetLayout, state.samplerSetLayout};
    const VkDescriptorSetAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = &countInfo,
        .descriptorPool = pool,
        .descriptorSetCount = 2,
        .pSetLayouts = layouts,
    };
    VkDescriptorSet sets[2] = {};
    checkVk(state.functions.vkAllocateDescriptorSets(state.device, &allocInfo, sets), "vkAllocateDescriptorSets");

    DescriptorHeap heap;
    heap.resourceCount = desc.resourceCount;
    heap.samplerCount = desc.samplerCount;
    heap.internal.pool = reinterpret_cast<uint64>(pool);
    heap.internal.resourceSet = reinterpret_cast<uint64>(sets[0]);
    heap.internal.samplerSet = reinterpret_cast<uint64>(sets[1]);
    heap.internal.id = EQ_VK_TRACK(&state);
    return heap;
}

void destroyDescriptorHeap(Device &device, DescriptorHeap &heap) {
    DeviceState &state = *device.internal.state;
    EQ_VK_CHECK_ALIVE(&state, heap.internal.id, "descriptor heap");
    EQ_VK_UNTRACK(&state, heap.internal.id);
    state.functions.vkDestroyDescriptorPool(state.device, native(heap).pool, nullptr);
    heap = {};
}

void writeDescriptors(Device &device, DescriptorHeap &heap, std::initializer_list<DescriptorWrite> writes) {
    writeDescriptors(device, heap, std::span<const DescriptorWrite>(writes.begin(), writes.size()));
}

void writeDescriptors(Device &device, DescriptorHeap &heap, std::span<const DescriptorWrite> writes) {
    DeviceState &state = *device.internal.state;
    EQ_VK_CHECK_ALIVE(&state, heap.internal.id, "descriptor heap");

    std::vector<VkDescriptorBufferInfo> bufferInfos(writes.size());
    std::vector<VkDescriptorImageInfo> imageInfos(writes.size());
    std::vector<VkWriteDescriptorSet> natives(writes.size());
    for (size_t i = 0; i < writes.size(); ++i) {
        const DescriptorWrite &write = writes[i];
        EQ_VALIDATE(Validation::checkSlot(write.slot, heap.resourceCount, "resource"));
        natives[i] = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = native(heap).resourceSet,
            .dstBinding = 0,
            .dstArrayElement = write.slot,
            .descriptorCount = 1,
        };

        // A read or read-write view names a texture when its texture view is set.
        const TextureView *texture = std::visit(
            [](const auto &view) -> const TextureView * {
                using View = std::decay_t<decltype(view)>;
                if constexpr (std::is_same_v<View, ConstantBufferView>) {
                    return nullptr;
                } else {
                    return view.texture.internal.view != 0 ? &view.texture : nullptr;
                }
            },
            write.descriptor);
        if (texture != nullptr) {
            EQ_VK_CHECK_ALIVE(&state, texture->internal.textureId, "texture");
            const bool storage = std::holds_alternative<ReadWriteView>(write.descriptor);
            constexpr uint32 kDepthStencil = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            if ((texture->internal.aspectMask & kDepthStencil) == kDepthStencil) {
                reportError(ErrorCategory::eProgrammerError,
                            "writeDescriptors: a view with both depth and stencil cannot be read by shaders; create "
                            "a view with Aspect::eDepth or Aspect::eStencil (P73)");
            }
            // Shaders access textures in their resting layout (L1): GENERAL for storage.
            imageInfos[i] = {.imageView = native(*texture),
                             .imageLayout = storage ? VK_IMAGE_LAYOUT_GENERAL
                                                    : static_cast<VkImageLayout>(texture->internal.restingLayout)};
            natives[i].descriptorType = storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            natives[i].pImageInfo = &imageInfos[i];
            continue;
        }

        // Buffer views differ in descriptor type.
        const auto [buffer, offset, size, stride, type] = std::visit(
            [](const auto &view) {
                using View = std::decay_t<decltype(view)>;
                if constexpr (std::is_same_v<View, ConstantBufferView>) {
                    return std::tuple{view.buffer, view.offset, view.size, uint32{0}, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
                } else {
                    return std::tuple{view.buffer, view.offset, view.size, view.stride, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
                }
            },
            write.descriptor);
        EQ_VK_CHECK_ALIVE(&state, buffer.internal.id, "buffer");
        const bool constant = type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        const uint64 resolved = size == kWholeSize ? buffer.size - std::min(offset, buffer.size) : size;
        EQ_VALIDATE(Validation::checkBufferView(offset, resolved, buffer.size, stride, constant));

        // Constant views cover whole 256-byte blocks on both backends.
        bufferInfos[i] = {.buffer = native(buffer),
                          .offset = offset,
                          .range = constant ? Validation::constantViewSize(resolved) : resolved};
        natives[i].descriptorType = type;
        natives[i].pBufferInfo = &bufferInfos[i];
    }
    // One native call for the whole list (D1, P94).
    state.functions.vkUpdateDescriptorSets(state.device, static_cast<uint32>(natives.size()), natives.data(), 0,
                                           nullptr);
}

// ── Queries (P51, P71) ───────────────────────────────────────────────────────

QueryPool createQueryPool(Device &device, const QueryPoolDesc &desc) {
    DeviceState &state = *device.internal.state;
    VkQueryPoolCreateInfo info{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryCount = desc.count};
    switch (desc.type) {
    case QueryType::eTimestamp: info.queryType = VK_QUERY_TYPE_TIMESTAMP; break;
    case QueryType::eOcclusion: info.queryType = VK_QUERY_TYPE_OCCLUSION; break;
    case QueryType::ePipelineStatistics:
        info.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        info.pipelineStatistics = kPipelineStatistics;
        break;
    }
    VkQueryPool pool = VK_NULL_HANDLE;
    checkVk(state.functions.vkCreateQueryPool(state.device, &info, nullptr, &pool), "vkCreateQueryPool");
    // Created reset, on the CPU (P71 (1)).
    state.functions.vkResetQueryPool(state.device, pool, 0, desc.count);

    QueryPool out;
    out.type = desc.type;
    out.count = desc.count;
    out.internal.device = &state;
    out.internal.pool = reinterpret_cast<uint64>(pool);
    out.internal.id = EQ_VK_TRACK(&state);
    return out;
}

void destroyQueryPool(Device &device, QueryPool &pool) {
    DeviceState &state = *device.internal.state;
    EQ_VK_CHECK_ALIVE(&state, pool.internal.id, "query pool");
    EQ_VK_UNTRACK(&state, pool.internal.id);
    state.functions.vkDestroyQueryPool(state.device, native(pool), nullptr);
    pool = {};
}

// Vulkan writes only the enabled counters, in bit order (P71 (2)).
PipelineStatisticsLayout pipelineStatisticsLayout(Device & /*device*/) {
    return {.size = 7 * 8,
            .inputVertices = 0,
            .inputPrimitives = 8,
            .vertexShaderInvocations = 16,
            .clippingInvocations = 24,
            .clippingPrimitives = 32,
            .pixelShaderInvocations = 40,
            .computeShaderInvocations = 48};
}

void resetQueries(Device &device, QueryPool &pool, uint32 first, uint32 count) {
    DeviceState &state = *device.internal.state;
    EQ_VK_CHECK_ALIVE(&state, pool.internal.id, "query pool");
    EQ_VALIDATE(Validation::checkRange(first, count, pool.count, 1, "resetQueries: the query range"));
    state.functions.vkResetQueryPool(state.device, native(pool), first, count);
}

// ── Import (P77) ─────────────────────────────────────────────────────────────

Memory importMemory(Device &device, VkDeviceMemory memory, MemoryKind kind, uint64 size, void *mapped) {
    DeviceState &state = *device.internal.state;
    Memory result;
    result.kind = kind;
    result.size = size;
    result.mapped = mapped;
    result.internal.memory = reinterpret_cast<uint64>(memory);
    result.internal.memoryType = state.memoryTypes[kindIndex(kind)];
    result.internal.imported = true;
    result.internal.id = EQ_VK_TRACK(&state);
    return result;
}

Buffer importBuffer(Device &device, VkBuffer buffer, const BufferDesc &desc, void *mapped) {
    DeviceState &state = *device.internal.state;
    const VkBufferDeviceAddressInfo addressInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffer};
    Buffer result;
    result.size = desc.size;
    result.usage = desc.usage;
    result.gpuAddress = state.functions.vkGetBufferDeviceAddress(state.device, &addressInfo);
    result.mapped = mapped;
    result.internal.buffer = reinterpret_cast<uint64>(buffer);
    result.internal.imported = true;
    result.internal.id = EQ_VK_TRACK(&state);
    return result;
}

void writeNativeDescriptor(Device &device, DescriptorHeap &heap, uint32 slot, VkDescriptorType type,
                           const VkDescriptorImageInfo *image, const VkDescriptorBufferInfo *buffer) {
    DeviceState &state = *device.internal.state;
    EQ_VK_CHECK_ALIVE(&state, heap.internal.id, "descriptor heap");
    EQ_VALIDATE(Validation::checkSlot(slot, heap.resourceCount, "resource"));
    const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                     .dstSet = native(heap).resourceSet,
                                     .dstBinding = 0,
                                     .dstArrayElement = slot,
                                     .descriptorCount = 1,
                                     .descriptorType = type,
                                     .pImageInfo = image,
                                     .pBufferInfo = buffer};
    state.functions.vkUpdateDescriptorSets(state.device, 1, &write, 0, nullptr);
}

} // namespace RI::Vk
