#pragma once

// Portable memory, buffer and descriptor descriptions (CONTEXT.md P31, P32,
// P33, P58, P59, P61, P87, P94). The objects themselves are per-backend structs
// (P83): RI::Vk::Buffer, RI::D3D12::Buffer, ...

#include <RI/Flags.hpp>
#include <types.hpp>

#include <cstddef>
#include <span>

namespace RI {

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
};
makeBitmask(BufferUsage)

struct BufferDesc {
    uint64 size = 0;
    BufferUsage usage = BufferUsage::eNone;
};

// Size and alignment a resource needs when placed in memory (P31, P58 (1)).
// Buffers fit every memory kind.
struct MemoryRequirements {
    uint64 size = 0;
    uint64 alignment = 0;
};

// Sizes of a descriptor heap pair (P59), within the device's limits.
struct DescriptorHeapDesc {
    uint32 resourceCount = 0; // read, read-write and constant-buffer views
    uint32 samplerCount = 0;
};

// The rest of a buffer from `offset` (view sizes).
inline constexpr uint64 kWholeSize = ~uint64{0};

// Shader bytecode: SPIR-V for Vulkan, DXIL for D3D12 (P29).
using Bytecode = std::span<const std::byte>;

struct ComputePipelineDesc {
    Bytecode shader; // entry point "main"
};

} // namespace RI
