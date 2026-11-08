#pragma once

// Native Metal access (P22, P84, P113 (3)): including this header and naming
// the backend (`RI::Metal::native(device)`) is the only way to reach Metal
// objects. The pointers are metal-cpp's; Objective-C code casts them to
// `id<MTL...>` with `(__bridge id)`, as they are the same objects.
//
// Not yet on Metal: native sections and the import of native objects (P27,
// P70, P77).

#include <RI/Metal/Backend.hpp>

#include <Metal/Metal.hpp>

namespace RI::Metal {

// Borrowed pointers: RI keeps its references; retain to keep one longer.
struct NativeDevice {
    MTL::Device *device = nullptr;
    MTL4::CommandQueue *graphicsQueue = nullptr;
    MTL::ResidencySet *residencySet = nullptr; // every Memory heap, heap buffer and command memory chunk
    MTL::SharedEvent *timeline = nullptr;      // the graphics queue's timeline (P20)
};

NativeDevice native(const Device &device);

// Native objects behind RI objects (P84). Borrowed pointers.
inline MTL::Heap *native(const Memory &memory) { return static_cast<MTL::Heap *>(memory.internal.heap); }
inline MTL::Buffer *native(const Buffer &buffer) { return static_cast<MTL::Buffer *>(buffer.internal.buffer); }
inline MTL4::CounterHeap *native(const QueryPool &pool) { return static_cast<MTL4::CounterHeap *>(pool.internal.heap); }

// The heap buffers, bound at argument-table buffer indices 0 and 1
// (kIRDescriptorHeapBindPoint, kIRSamplerHeapBindPoint), each an array of
// 24-byte IRDescriptorTableEntry (P114 (3)).
struct NativeDescriptorHeap {
    MTL::Buffer *resources = nullptr;
    MTL::Buffer *samplers = nullptr;
};
inline NativeDescriptorHeap native(const DescriptorHeap &heap) {
    return {.resources = static_cast<MTL::Buffer *>(heap.internal.resourceBuffer),
            .samplers = static_cast<MTL::Buffer *>(heap.internal.samplerBuffer)};
}

// A compute pipeline's state; graphics pipelines are not built yet on Metal.
inline MTL::ComputePipelineState *native(const Pipeline &pipeline) {
    return pipeline.internal.graphics ? nullptr : static_cast<MTL::ComputePipelineState *>(pipeline.internal.pipelineState);
}

// The command buffer being recorded. RI opens and closes encoders itself.
MTL4::CommandBuffer *native(const CommandList &cmd);

} // namespace RI::Metal
