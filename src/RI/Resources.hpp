#pragma once

// Portable memory, buffer and descriptor descriptions (CONTEXT.md P31, P32,
// P33, P58, P59, P61, P87, P94). The objects themselves are per-backend structs
// (P83): RI::Vk::Buffer, RI::D3D12::Buffer, ...

#include <RI/Abi.hpp>
#include <RI/Flags.hpp>
#include <types.hpp>

#include <cstddef>
#include <span>
#include <string_view>

namespace RI::inline EQUINE_RI_ABI {

// The memory kinds (P32). Each backend maps them to its native memory types.
enum class MemoryKind : uint8 {
    eGPU,       // GPU-only; never mapped (P33)
    eUpload,    // CPU writes, GPU reads; mapped for its lifetime, write-only in practice
    eReadback,  // GPU writes, CPU reads; mapped for its lifetime
    eGPUUpload, // GPU memory the CPU can write (ReBAR); availability varies (P32)
};

// What a buffer may be used for (P58). Read and read-write views are shader
// accesses through descriptors (P87); GPU addresses are always available (P61).
enum class BufferUsage : uint32 {
    eNone = 0,
    eRead = rvl::eBit(0),      // ReadView: StructuredBuffer, ByteAddressBuffer
    eReadWrite = rvl::eBit(1), // ReadWriteView: RWStructuredBuffer, RWByteAddressBuffer
    eConstant = rvl::eBit(2),  // ConstantBufferView: ConstantBuffer<T>
    eCopySrc = rvl::eBit(3),
    eCopyDst = rvl::eBit(4),
    eIndirect = rvl::eBit(5), // indirect arguments (P48)
    eIndex = rvl::eBit(6),    // index buffer for cmdDrawIndexed (P39)
    eAccelerationStructureInput = rvl::eBit(7), // vertices, indices or instances of a build (P49)
    eIndirectScratch = rvl::eBit(8), // scratch of cmdExecuteIndirectWithRootData (P48)
};
makeBitmask(BufferUsage)

struct BufferDesc {
    uint64 size = 0;
    BufferUsage usage = BufferUsage::eNone;
    std::string_view name; // empty: unnamed (P106)
};

// Size, alignment and compatible memory kinds of a resource placed in memory
// (P31, P58 (1)). Buffers fit every available kind; textures usually only eGPU.
struct MemoryRequirements {
    uint64 size = 0;
    uint64 alignment = 0;
    uint32 kinds = 0; // bit i set: MemoryKind i can hold the resource
    struct {
        uint32 memoryTypes = 0; // Vulkan: the native types allowed (memoryTypeBits)
    } internal;
};

inline constexpr bool fits(const MemoryRequirements &requirements, MemoryKind kind) {
    return (requirements.kinds & (1u << static_cast<uint32>(kind))) != 0;
}

// Sizes of a descriptor heap pair (P59), within the device's limits.
struct DescriptorHeapDesc {
    uint32 resourceCount = 0; // read, read-write and constant-buffer views
    uint32 samplerCount = 0;
    std::string_view name; // empty: unnamed (P106)
};

// The rest of a buffer from `offset` (view sizes).
inline constexpr uint64 kWholeSize = ~uint64{0};

// The most elements one read or read-write buffer view covers, on both
// backends (P106: D3D12's limit). Raw views count 4-byte elements, so at most
// 512 MiB; structured views count `stride`. DeviceLimits::maxBufferViewSize
// and maxConstantViewSize bound the bytes.
inline constexpr uint64 kMaxBufferViewElements = uint64{1} << 27;

// Shader bytecode: SPIR-V for Vulkan, DXIL for D3D12 (P29).
using Bytecode = std::span<const std::byte>;

struct ComputePipelineDesc {
    Bytecode shader; // entry point "main"
    std::string_view name; // empty: unnamed (P106)
};

// Work graphs (P50, P62), D3D12 only: a DXIL library (lib_6_8) whose node
// shaders form the graph. RI does not read root data from libraries (P62), so
// give the size of the EQ_ROOT_DATA block the nodes declare (0: none).
struct WorkGraphDesc {
    Bytecode library;
    uint32 rootDataSize = 0;
    // A promise that lowers WorkGraph::backing (P111): at most maxInputRecords
    // records and maxNodeInputs entry nodes per cmdDispatchGraph. Both or
    // neither (0: no promise). Needs an Agility SDK preview (P110).
    uint32 maxInputRecords = 0;
    uint32 maxNodeInputs = 0;
    std::string_view name; // empty: unnamed (P106)
};

// Backing memory a work graph needs (P50): a range of a buffer with
// eReadWrite, between minSize and maxSize bytes (larger can be faster).
struct WorkGraphMemory {
    uint64 minSize = 0;
    uint64 maxSize = 0;
};

// CPU input records of one entry node (P62).
struct GraphInput {
    uint32 entry = 0; // workGraphEntry(graph, nodeName)
    const void *records = nullptr;
    uint32 recordCount = 0;
    uint32 recordStride = 0;
};

} // namespace RI
