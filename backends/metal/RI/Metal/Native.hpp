#pragma once

// Native Metal access (P22, P84, P113 (3)): including this header and naming
// the backend (`RI::Metal::native(device)`) is the only way to reach Metal
// objects. The pointers are metal-cpp's; Objective-C code casts them to
// `id<MTL...>` with `(__bridge id)`, as they are the same objects.
//

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
inline MTL::Texture *native(const Texture &texture) { return static_cast<MTL::Texture *>(texture.internal.texture); }
// The view's own texture (a Metal texture view), of the type shaders read it as.
inline MTL::Texture *native(const TextureView &view) { return static_cast<MTL::Texture *>(view.internal.texture); }
inline MTL::AccelerationStructure *native(const AccelerationStructure &structure) {
    return static_cast<MTL::AccelerationStructure *>(structure.internal.structure);
}
// A pool's storage: `counters` for timestamp pools; `results` for occlusion
// pools, a private buffer of one 64-bit count per query (visibility results are
// copied there at the end of each section that measured them).
struct NativeQueryPool {
    MTL4::CounterHeap *counters = nullptr;
    MTL::Buffer *results = nullptr;
};
inline NativeQueryPool native(const QueryPool &pool) {
    if (pool.type == QueryType::eOcclusion) {
        return {.results = static_cast<MTL::Buffer *>(pool.internal.heap)};
    }
    return {.counters = static_cast<MTL4::CounterHeap *>(pool.internal.heap)};
}

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

// A pipeline's states: `compute` for compute pipelines; `render` (from an
// MTLRenderPipelineDescriptor, or an MTLMeshRenderPipelineDescriptor for mesh
// pipelines) and `depthStencil` for graphics ones. The rest of the graphics
// state (cull mode, winding, fill mode, depth bias and clip mode) is render
// encoder state on Metal, set by cmdSetPipeline.
struct NativePipeline {
    MTL::ComputePipelineState *compute = nullptr;
    MTL::RenderPipelineState *render = nullptr;
    MTL::DepthStencilState *depthStencil = nullptr;
};
inline NativePipeline native(const Pipeline &pipeline) {
    if (!pipeline.internal.graphics) {
        return {.compute = static_cast<MTL::ComputePipelineState *>(pipeline.internal.pipelineState)};
    }
    return {.render = static_cast<MTL::RenderPipelineState *>(pipeline.internal.pipelineState),
            .depthStencil = static_cast<MTL::DepthStencilState *>(pipeline.internal.depthStencilState)};
}

// The command buffer being recorded (cmdBegin may replace it once the last
// submission completed: fetch it per recording). RI opens and closes encoders
// itself: record native commands in a native section.
MTL4::CommandBuffer *native(const CommandList &cmd);

// ── Native sections (P27, P70, P118) ──────────────────────────────────────────
// Native commands in the middle of a command list. Exactly one field is set:
// - Outside a rendering section, `commandBuffer`: RI has ended its encoder;
//   open and end your own (compute, render, your indirect command buffers
//   with executeCommandsInBuffer), and leave none open at endNative.
// - Inside a rendering section, `renderEncoder`: the section's encoder. Do
//   not end it, change attachments or set its visibility result mode (the
//   section's visibility buffer is RI's occlusion queries').
// Record no RI commands in between. Synchronization with RI's commands is
// yours, a native barrier or cmdBarrier, on either side of the section. What
// the GPU reads must be resident: keep your own MTL::ResidencySet, added to the
// queue (native(device).graphicsQueue) or the command buffer and committed by
// you; RI's set is RI's, not to change. Metal has no image layouts: there is
// nothing to hand back (P118 (3)).
struct NativeCommands {
    MTL4::CommandBuffer *commandBuffer = nullptr;
    MTL4::RenderCommandEncoder *renderEncoder = nullptr;
};
NativeCommands beginNative(CommandList &cmd);
// Sets RI's state again (P70 (1)): inside a rendering section, the argument
// table on every render stage (heaps, root data, draw arguments), the last
// graphics pipeline and its encoder state (depth-stencil, cull mode, winding,
// fill mode, depth clip and bias), the viewport, scissor and stencil
// reference. Outside sections, RI's next encoder starts from its own state.
void endNative(CommandList &cmd);

// ── Import (P77, P119) ────────────────────────────────────────────────────────
// Natively created objects as RI objects. RI holds no reference and releases
// nothing: importing joins the object to RI's residency set, and destroying
// the RI object only removes it from the set and forgets it (RI's own views of
// an imported texture are released). Keep the native object alive meanwhile.

// A placement heap (MTLHeapTypePlacement) with the storage mode of `kind`:
// private for eGPU, shared for the others (best write-combined for eUpload and
// eGPUUpload). RI maps CPU kinds through a buffer it places over the whole heap.
Memory importMemory(Device &device, MTL::Heap *heap, MemoryKind kind);
// At least desc.size bytes long. Mapped (contents()) unless its storage is private.
Buffer importBuffer(Device &device, MTL::Buffer *buffer, const BufferDesc &desc);
// A texture matching `desc` (type, pixel format, size, mips, layers, samples),
// in private storage, with at least the MTLTextureUsage RI gives desc.usage
// (and PixelFormatView when desc.viewFormats lists formats). No layout to
// bring it into: Metal has none.
Texture importTexture(Device &device, MTL::Texture *texture, const TextureDesc &desc);

// Pipelines that follow RI's binding model (P114): shaders converted by Metal
// Shader Converter with RI's root signature, or written to the same argument
// table layout. Metal keeps state outside its pipeline objects that Vulkan and
// D3D12 keep inside, so it is passed here. Geometry and tessellation pipelines
// (RI's emulation) cannot be imported; an imported pipeline skips the
// read-only depth and stencil checks (P75), as on D3D12.
//
// Compute: Metal takes the threadgroup size (numthreads) at each dispatch.
Pipeline importPipeline(Device &device, MTL::ComputePipelineState *pipeline, uint32 rootDataSize,
                        MTL::Size threadGroupSize);
// Graphics: the render encoder state cmdSetPipeline sets, Metal's defaults
// (and RI's clockwise front faces) when left out.
struct NativeRenderState {
    MTL::DepthStencilState *depthStencil = nullptr; // nullptr: no depth or stencil test
    MTL::PrimitiveType primitiveType = MTL::PrimitiveTypeTriangle;
    MTL::CullMode cullMode = MTL::CullModeNone;
    MTL::Winding winding = MTL::WindingClockwise;
    MTL::TriangleFillMode fillMode = MTL::TriangleFillModeFill;
    MTL::DepthClipMode depthClipMode = MTL::DepthClipModeClip;
    float depthBias = 0.0f, depthSlopeScale = 0.0f, depthBiasClamp = 0.0f;
    uint8 stencilReference = 0;
    // Mesh pipelines (drawn with cmdDrawMesh): the object (task) and mesh
    // stages' threadgroup sizes; the object size is ignored without an object stage.
    bool mesh = false;
    MTL::Size objectThreadGroupSize{1, 1, 1};
    MTL::Size meshThreadGroupSize{1, 1, 1};
};
Pipeline importPipeline(Device &device, MTL::RenderPipelineState *pipeline, uint32 rootDataSize,
                        const NativeRenderState &state = {});

// A raw heap entry (P77 (2)): Metal Shader Converter's IRDescriptorTableEntry,
// for views RI does not model. What it points at must be resident: in RI's
// memory, imported, or in your own residency set.
struct NativeDescriptor {
    uint64 gpuVA = 0;
    uint64 textureViewID = 0;
    uint64 metadata = 0;
};
void writeNativeDescriptor(Device &device, DescriptorHeap &heap, uint32 slot, const NativeDescriptor &entry);

} // namespace RI::Metal
