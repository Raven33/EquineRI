// Inline ray tracing on Vulkan (P49).
//
// Owns: acceleration structures in user memory (a buffer plus a
// VkAccelerationStructureKHR), size queries, and cmdBuildBlas / cmdBuildTlas
// (with update). Shaders read the top level from a heap slot with RayQuery.
//
// Watch out: needs DeviceLimits::rayTracing, which Device.cpp turns off when
// acceleration structures cannot be mutable descriptors (AMD Windows). Scratch
// addresses must be aligned to accelerationStructureScratchAlignment.
//
// D3D12 counterpart: backends/d3d12/RayTracing.cpp.

#include "DeviceState.hpp"
#include "VkCheck.hpp"

#include <RI/Error.hpp>
#include <RI/Validation/Checks.hpp>

#include <format>
#include <vector>

namespace RI::Vk {

namespace {

void requireRayTracing(const DeviceState &state, const char *call) {
    if (!state.rayTracing) {
        reportError(ErrorCategory::eUnsupported,
                    std::format("{}: this GPU has no inline ray tracing (DeviceLimits::rayTracing)", call));
    }
}

VkBuildAccelerationStructureFlagsKHR flagsOf(AccelerationStructureFlags flags) {
    VkBuildAccelerationStructureFlagsKHR out = 0;
    if ((flags & AccelerationStructureFlags::ePreferFastTrace) != AccelerationStructureFlags::eNone) {
        out |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    }
    if ((flags & AccelerationStructureFlags::ePreferFastBuild) != AccelerationStructureFlags::eNone) {
        out |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    }
    if ((flags & AccelerationStructureFlags::eAllowUpdate) != AccelerationStructureFlags::eNone) {
        out |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    }
    return out;
}

// The native geometry of a build, kept alive with its primitive counts.
struct Geometry {
    std::vector<VkAccelerationStructureGeometryKHR> geometries;
    std::vector<uint32> primitiveCounts;
    VkAccelerationStructureBuildGeometryInfoKHR info{};
};

Geometry blasGeometry(const BlasDesc &desc) {
    Geometry g;
    for (const BlasTriangles &t : desc.triangles) {
        VkAccelerationStructureGeometryTrianglesDataKHR triangles{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
            .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
            .vertexData = {.deviceAddress = t.vertexAddress},
            .vertexStride = t.vertexStride,
            .maxVertex = t.vertexCount > 0 ? t.vertexCount - 1 : 0,
            .indexType = t.indexAddress == 0               ? VK_INDEX_TYPE_NONE_KHR
                         : t.indexFormat == IndexFormat::eUint16 ? VK_INDEX_TYPE_UINT16
                                                               : VK_INDEX_TYPE_UINT32,
            .indexData = {.deviceAddress = t.indexAddress},
        };
        g.geometries.push_back({.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                                .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
                                .geometry = {.triangles = triangles},
                                .flags = t.opaque ? VkGeometryFlagsKHR{VK_GEOMETRY_OPAQUE_BIT_KHR} : 0u});
        g.primitiveCounts.push_back((t.indexAddress != 0 ? t.indexCount : t.vertexCount) / 3);
    }
    g.info = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
              .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
              .flags = flagsOf(desc.flags),
              .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
              .geometryCount = static_cast<uint32>(g.geometries.size()),
              .pGeometries = g.geometries.data()};
    return g;
}

Geometry tlasGeometry(const TlasDesc &desc) {
    Geometry g;
    VkAccelerationStructureGeometryInstancesDataKHR instances{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
        .arrayOfPointers = VK_FALSE,
        .data = {.deviceAddress = desc.instancesAddress}};
    g.geometries.push_back({.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                            .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
                            .geometry = {.instances = instances}});
    g.primitiveCounts.push_back(desc.instanceCount);
    g.info = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
              .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
              .flags = flagsOf(desc.flags),
              .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
              .geometryCount = 1,
              .pGeometries = g.geometries.data()};
    return g;
}

AccelerationStructureSizes sizesOf(DeviceState &state, Geometry &g) {
    VkAccelerationStructureBuildSizesInfoKHR sizes{.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    state.functions.vkGetAccelerationStructureBuildSizesKHR(state.device,
                                                           VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &g.info,
                                                           g.primitiveCounts.data(), &sizes);
    return {.size = sizes.accelerationStructureSize,
            .buildScratchSize = sizes.buildScratchSize,
            .updateScratchSize = sizes.updateScratchSize};
}

VkBufferCreateInfo storageBufferInfo(uint64 size) {
    return {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
}

void build(CommandList &cmd, const AccelerationStructure &dst, Geometry &g, const Buffer &scratch,
           uint64 scratchOffset, bool update, const char *call) {
    DeviceState &state = *cmd.internal.device;
    requireRayTracing(state, call);
    EQ_VK_CHECK_ALIVE(&state, dst.internal.id, "acceleration structure");
    EQ_VK_CHECK_ALIVE(&state, scratch.internal.id, "buffer");
    EQ_VALIDATE(Validation::checkRenderingSection(cmd.internal.rendering, false, call));
#if EQUINE_VALIDATION
    const AccelerationStructureSizes sizes = sizesOf(state, g);
#endif
    EQ_VALIDATE(Validation::checkRange(scratchOffset, update ? sizes.updateScratchSize : sizes.buildScratchSize,
                                       scratch.size, 1, std::format("{}: the scratch range", call)));
    // The scratch address (not only the offset) must be aligned: place
    // scratch buffers on DeviceLimits::accelerationStructureScratchAlignment.
    EQ_VALIDATE(Validation::checkRange(scratch.gpuAddress + scratchOffset, 0, UINT64_MAX, state.scratchAlignment,
                                       std::format("{}: the scratch address (buffer GPU address + offset)", call)));
    const auto handle = reinterpret_cast<VkAccelerationStructureKHR>(dst.internal.handle);
    g.info.mode = update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    g.info.srcAccelerationStructure = update ? handle : VK_NULL_HANDLE;
    g.info.dstAccelerationStructure = handle;
    g.info.scratchData.deviceAddress = scratch.gpuAddress + scratchOffset;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
    for (uint32 count : g.primitiveCounts) {
        ranges.push_back({.primitiveCount = count});
    }
    const VkAccelerationStructureBuildRangeInfoKHR *rangePointer = ranges.data();
    state.functions.vkCmdBuildAccelerationStructuresKHR(static_cast<VkCommandBuffer>(cmd.internal.commandBuffer), 1,
                                                        &g.info, &rangePointer);
}

} // namespace

AccelerationStructureSizes blasSizes(Device &device, const BlasDesc &desc) {
    DeviceState &state = *device.internal.state;
    requireRayTracing(state, "blasSizes");
    Geometry g = blasGeometry(desc);
    return sizesOf(state, g);
}

AccelerationStructureSizes tlasSizes(Device &device, const TlasDesc &desc) {
    DeviceState &state = *device.internal.state;
    requireRayTracing(state, "tlasSizes");
    Geometry g = tlasGeometry(desc);
    return sizesOf(state, g);
}

// The structure's buffer is placed like any buffer; the structure itself must
// start on 256 bytes inside it, which offset 0 does.
MemoryRequirements accelerationStructureRequirements(Device &device, uint64 size) {
    DeviceState &state = *device.internal.state;
    requireRayTracing(state, "accelerationStructureRequirements");
    const VkBufferCreateInfo info = storageBufferInfo(size);
    const VkDeviceBufferMemoryRequirements query{.sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS,
                                                 .pCreateInfo = &info};
    VkMemoryRequirements2 requirements{.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    state.functions.vkGetDeviceBufferMemoryRequirements(state.device, &query, &requirements);
    MemoryRequirements out{.size = requirements.memoryRequirements.size,
                           .alignment = std::max<uint64>(requirements.memoryRequirements.alignment, 256),
                           .kinds = kindsOf(state, requirements.memoryRequirements.memoryTypeBits),
                           .internal = {.memoryTypes = requirements.memoryRequirements.memoryTypeBits}};
    return withGranularity(state, out);
}

AccelerationStructure createAccelerationStructure(Device &device, const Memory &memory, uint64 offset, uint64 size,
                                                  bool topLevel) {
    DeviceState &state = *device.internal.state;
    requireRayTracing(state, "createAccelerationStructure");
    EQ_VK_CHECK_ALIVE(&state, memory.internal.id, "memory");
    [[maybe_unused]] const MemoryRequirements requirements = accelerationStructureRequirements(device, size);
    EQ_VALIDATE(Validation::checkRange(offset, requirements.size, memory.size, requirements.alignment,
                                       "createAccelerationStructure: placing it in memory"));
    EQ_VALIDATE(Validation::checkMemoryType(
        memory.internal.imported || (requirements.internal.memoryTypes & (1u << memory.internal.memoryType)) != 0,
        "createAccelerationStructure"));
    const VkBufferCreateInfo bufferInfo = storageBufferInfo(size);
    VkBuffer buffer = VK_NULL_HANDLE;
    checkVk(state.functions.vkCreateBuffer(state.device, &bufferInfo, nullptr, &buffer), "vkCreateBuffer");
    checkVk(state.functions.vkBindBufferMemory(state.device, buffer, native(memory), offset), "vkBindBufferMemory");
    const VkAccelerationStructureCreateInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
        .buffer = buffer,
        .size = size,
        .type = topLevel ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR};
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    checkVk(state.functions.vkCreateAccelerationStructureKHR(state.device, &info, nullptr, &handle),
            "vkCreateAccelerationStructureKHR");
    const VkAccelerationStructureDeviceAddressInfoKHR addressInfo{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR, .accelerationStructure = handle};
    AccelerationStructure out;
    out.size = size;
    out.gpuAddress = state.functions.vkGetAccelerationStructureDeviceAddressKHR(state.device, &addressInfo);
    out.topLevel = topLevel;
    out.internal.handle = reinterpret_cast<uint64>(handle);
    out.internal.buffer = reinterpret_cast<uint64>(buffer);
    out.internal.id = EQ_VK_TRACK(&state);
    return out;
}

void destroyAccelerationStructure(Device &device, AccelerationStructure &structure) {
    DeviceState &state = *device.internal.state;
    EQ_VK_CHECK_ALIVE(&state, structure.internal.id, "acceleration structure");
    EQ_VK_UNTRACK(&state, structure.internal.id);
    state.functions.vkDestroyAccelerationStructureKHR(
        state.device, reinterpret_cast<VkAccelerationStructureKHR>(structure.internal.handle), nullptr);
    state.functions.vkDestroyBuffer(state.device, reinterpret_cast<VkBuffer>(structure.internal.buffer), nullptr);
    structure = {};
}

void cmdBuildBlas(CommandList &cmd, const AccelerationStructure &dst, const BlasDesc &desc, const Buffer &scratch,
                  uint64 scratchOffset, bool update) {
    Geometry g = blasGeometry(desc);
    build(cmd, dst, g, scratch, scratchOffset, update, "cmdBuildBlas");
}

void cmdBuildTlas(CommandList &cmd, const AccelerationStructure &dst, const TlasDesc &desc, const Buffer &scratch,
                  uint64 scratchOffset, bool update) {
    Geometry g = tlasGeometry(desc);
    build(cmd, dst, g, scratch, scratchOffset, update, "cmdBuildTlas");
}

} // namespace RI::Vk
