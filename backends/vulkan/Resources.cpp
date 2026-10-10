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
    result.internal.id = EQ_VK_TRACK(&state);
    return result;
}

void freeMemory(Device &device, Memory &memory) {
    DeviceState &state = *device.internal.state;
    EQ_VK_CHECK_ALIVE(&state, memory.internal.id, "memory");
    EQ_VK_UNTRACK(&state, memory.internal.id);
    state.functions.vkFreeMemory(state.device, native(memory), nullptr);
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
    return {.size = requirements.memoryRequirements.size, .alignment = requirements.memoryRequirements.alignment};
}

Buffer createBuffer(Device &device, const Memory &memory, uint64 offset, const BufferDesc &desc) {
    DeviceState &state = *device.internal.state;
    EQ_VK_CHECK_ALIVE(&state, memory.internal.id, "memory");
    const MemoryRequirements requirements = bufferRequirements(device, desc);
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
    return result;
}

void destroyBuffer(Device &device, Buffer &buffer) {
    DeviceState &state = *device.internal.state;
    EQ_VK_CHECK_ALIVE(&state, buffer.internal.id, "buffer");
    EQ_VK_UNTRACK(&state, buffer.internal.id);
    state.functions.vkDestroyBuffer(state.device, native(buffer), nullptr);
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

    std::vector<VkDescriptorBufferInfo> infos(writes.size());
    std::vector<VkWriteDescriptorSet> natives(writes.size());
    for (size_t i = 0; i < writes.size(); ++i) {
        const DescriptorWrite &write = writes[i];
        EQ_VALIDATE(Validation::checkSlot(write.slot, heap.resourceCount, "resource"));

        // All three views are buffer ranges; they differ in descriptor type.
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
        infos[i] = {.buffer = native(buffer),
                    .offset = offset,
                    .range = constant ? Validation::constantViewSize(resolved) : resolved};
        natives[i] = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = native(heap).resourceSet,
            .dstBinding = 0,
            .dstArrayElement = write.slot,
            .descriptorCount = 1,
            .descriptorType = type,
            .pBufferInfo = &infos[i],
        };
    }
    // One native call for the whole list (D1, P94).
    state.functions.vkUpdateDescriptorSets(state.device, static_cast<uint32>(natives.size()), natives.data(), 0,
                                           nullptr);
}

} // namespace RI::Vk
