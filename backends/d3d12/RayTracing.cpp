// Inline ray tracing on D3D12 (P49).
//
// Owns: acceleration structures in placed buffers (ALLOW_UNORDERED_ACCESS |
// RAYTRACING_ACCELERATION_STRUCTURE), size queries, and cmdBuildBlas /
// cmdBuildTlas (with update) through BuildRaytracingAccelerationStructure.
// Shaders use RayQuery (RaytracingTier 1.1).
//
// Vulkan counterpart: backends/vulkan/RayTracing.cpp.

#include "D3D12Check.hpp"
#include "DeviceState.hpp"

#include <RI/Error.hpp>
#include <RI/Validation/Checks.hpp>

#include <format>
#include <vector>

namespace RI::inline EQUINE_RI_ABI::D3D12 {

namespace {

void requireRayTracing(const DeviceState &state, const char *call) {
    if (!state.rayTracing) {
        reportError(ErrorCategory::eUnsupported,
                    std::format("{}: this GPU has no inline ray tracing (DeviceLimits::rayTracing)", call));
    }
}

D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flagsOf(AccelerationStructureFlags flags) {
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS out = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
    if ((flags & AccelerationStructureFlags::ePreferFastTrace) != AccelerationStructureFlags::eNone) {
        out |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    }
    if ((flags & AccelerationStructureFlags::ePreferFastBuild) != AccelerationStructureFlags::eNone) {
        out |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    }
    if ((flags & AccelerationStructureFlags::eAllowUpdate) != AccelerationStructureFlags::eNone) {
        out |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    }
    return out;
}

// The native inputs of a build, kept alive with their geometry descriptions.
struct Inputs {
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
};

Inputs blasInputs(const BlasDesc &desc) {
    Inputs in;
    for (const BlasTriangles &t : desc.triangles) {
        D3D12_RAYTRACING_GEOMETRY_DESC geometry{.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES,
                                                .Flags = t.opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE
                                                                  : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE};
        geometry.Triangles = {
            .Transform3x4 = 0,
            .IndexFormat = t.indexAddress == 0                   ? DXGI_FORMAT_UNKNOWN
                           : t.indexFormat == IndexFormat::eUint16 ? DXGI_FORMAT_R16_UINT
                                                                 : DXGI_FORMAT_R32_UINT,
            .VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT,
            .IndexCount = t.indexAddress != 0 ? t.indexCount : 0,
            .VertexCount = t.vertexCount,
            .IndexBuffer = t.indexAddress,
            .VertexBuffer = {.StartAddress = t.vertexAddress, .StrideInBytes = t.vertexStride},
        };
        in.geometries.push_back(geometry);
    }
    in.inputs = {.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL,
                 .Flags = flagsOf(desc.flags),
                 .NumDescs = static_cast<UINT>(in.geometries.size()),
                 .DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY};
    in.inputs.pGeometryDescs = in.geometries.data();
    return in;
}

Inputs tlasInputs(const TlasDesc &desc) {
    Inputs in;
    in.inputs = {.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL,
                 .Flags = flagsOf(desc.flags),
                 .NumDescs = desc.instanceCount,
                 .DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY};
    in.inputs.InstanceDescs = desc.instancesAddress;
    return in;
}

AccelerationStructureSizes sizesOf(DeviceState &state, const Inputs &in) {
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    state.device->GetRaytracingAccelerationStructurePrebuildInfo(&in.inputs, &info);
    return {.size = info.ResultDataMaxSizeInBytes,
            .buildScratchSize = info.ScratchDataSizeInBytes,
            .updateScratchSize = info.UpdateScratchDataSizeInBytes};
}

D3D12_RESOURCE_DESC1 storageDesc(uint64 size) {
    return {.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
            .Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
            .Width = std::max<uint64>(size, 1),
            .Height = 1,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .Format = DXGI_FORMAT_UNKNOWN,
            .SampleDesc = {.Count = 1, .Quality = 0},
            .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
            .Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE};
}

void build(CommandList &cmd, const AccelerationStructure &dst, Inputs &in, const Buffer &scratch, uint64 scratchOffset,
           bool update, const char *call) {
    DeviceState &state = *cmd.internal.device;
    requireRayTracing(state, call);
    EQ_DX_CHECK_ALIVE(&state, dst.internal.id, "acceleration structure");
    EQ_DX_CHECK_ALIVE(&state, scratch.internal.id, "buffer");
    if (cmd.internal.rendering) {
        reportError(ErrorCategory::eProgrammerError, std::format("{}: not allowed inside a rendering section", call));
    }
#if EQUINE_VALIDATION
    const AccelerationStructureSizes sizes = sizesOf(state, in);
    EQ_VALIDATE(Validation::checkRange(scratchOffset, update ? sizes.updateScratchSize : sizes.buildScratchSize,
                                       scratch.size, 1, std::format("{}: the scratch range", call)));
    EQ_VALIDATE(Validation::checkRange(scratch.gpuAddress + scratchOffset, 0, UINT64_MAX,
                                       D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT,
                                       std::format("{}: the scratch address (buffer GPU address + offset)", call)));
#endif
    if (update) {
        in.inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
    }
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{
        .DestAccelerationStructureData = dst.gpuAddress,
        .Inputs = in.inputs,
        .SourceAccelerationStructureData = update ? dst.gpuAddress : 0,
        .ScratchAccelerationStructureData = scratch.gpuAddress + scratchOffset};
    static_cast<ID3D12GraphicsCommandList7 *>(cmd.internal.list)->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
}

} // namespace

AccelerationStructureSizes blasSizes(Device &device, const BlasDesc &desc) {
    DeviceState &state = *device.internal.state;
    requireRayTracing(state, "blasSizes");
    return sizesOf(state, blasInputs(desc));
}

AccelerationStructureSizes tlasSizes(Device &device, const TlasDesc &desc) {
    DeviceState &state = *device.internal.state;
    requireRayTracing(state, "tlasSizes");
    return sizesOf(state, tlasInputs(desc));
}

// A placed buffer: 64 KiB placement, sizes rounded likewise; eGPU memory.
MemoryRequirements accelerationStructureRequirements(Device &device, uint64 size) {
    requireRayTracing(*device.internal.state, "accelerationStructureRequirements");
    const uint64 alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    return {.size = (std::max<uint64>(size, 1) + alignment - 1) / alignment * alignment,
            .alignment = alignment,
            .kinds = 1u << static_cast<uint32>(MemoryKind::eGPU)};
}

AccelerationStructure createAccelerationStructure(Device &device, const Memory &memory, uint64 offset, uint64 size,
                                                  bool topLevel, std::string_view name) {
    EQ_DX_CHECK_THREAD(device.internal.state);
    DeviceState &state = *device.internal.state;
    requireRayTracing(state, "createAccelerationStructure");
    EQ_DX_CHECK_ALIVE(&state, memory.internal.id, "memory");
    [[maybe_unused]] const MemoryRequirements requirements = accelerationStructureRequirements(device, size);
    EQ_VALIDATE(Validation::checkRange(offset, requirements.size, (memory.size + 65535) / 65536 * 65536,
                                       requirements.alignment, "createAccelerationStructure: placing it in memory"));
    const D3D12_RESOURCE_DESC1 desc = storageDesc(size);
    ID3D12Resource *resource = nullptr;
    checkHr(state.device->CreatePlacedResource2(static_cast<ID3D12Heap *>(memory.internal.heap), offset, &desc,
                                                D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, 0, nullptr,
                                                IID_PPV_ARGS(&resource)),
            "CreatePlacedResource2 (acceleration structure)");
    AccelerationStructure out;
    out.size = size;
    out.gpuAddress = resource->GetGPUVirtualAddress();
    out.topLevel = topLevel;
    out.internal.resource = resource;
    out.internal.id = EQ_DX_TRACK(&state);
    if (!name.empty()) {
        applyName(*device.internal.state, out, name);
    }
    return out;
}

void destroyAccelerationStructure(Device &device, AccelerationStructure &structure) {
    EQ_DX_CHECK_THREAD(device.internal.state);
    [[maybe_unused]] DeviceState &state = *device.internal.state;
    EQ_DX_CHECK_ALIVE(&state, structure.internal.id, "acceleration structure");
    EQ_DX_UNTRACK(&state, structure.internal.id);
    auto *resource = static_cast<ID3D12Resource *>(structure.internal.resource);
    release(resource);
    structure = {};
}

void cmdBuildBlas(CommandList &cmd, const AccelerationStructure &dst, const BlasDesc &desc, const Buffer &scratch,
                  uint64 scratchOffset, bool update) {
    EQ_DX_CHECK_THREAD(cmd.internal.device);
    Inputs in = blasInputs(desc);
    build(cmd, dst, in, scratch, scratchOffset, update, "cmdBuildBlas");
}

void cmdBuildTlas(CommandList &cmd, const AccelerationStructure &dst, const TlasDesc &desc, const Buffer &scratch,
                  uint64 scratchOffset, bool update) {
    EQ_DX_CHECK_THREAD(cmd.internal.device);
    Inputs in = tlasInputs(desc);
    build(cmd, dst, in, scratch, scratchOffset, update, "cmdBuildTlas");
}

} // namespace RI::D3D12
