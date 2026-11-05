#pragma once

// Native D3D12 access (P22, P84): including this header and naming the backend
// (`RI::D3D12::native(device)`) is the only way to reach D3D12 objects. Also
// the device-creation hook, native sections (P27, P70) and import of native
// objects (P77).

#include <RI/Abi.hpp>
#include <RI/D3D12/Backend.hpp>

#include <d3d12.h>
#include <dxgi1_6.h>

#include <span>

namespace RI::inline EQUINE_RI_ABI::D3D12 {

// Borrowed pointers: RI keeps its references; AddRef to keep one longer.
struct NativeDevice {
    IDXGIFactory6 *factory = nullptr;
    IDXGIAdapter1 *adapter = nullptr;
    ID3D12Device *device = nullptr;
    ID3D12CommandQueue *graphicsQueue = nullptr; // a DIRECT queue
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_12_0;
};

NativeDevice native(const Device &device);

// Native objects behind RI objects (P84). Borrowed pointers.
inline ID3D12Heap *native(const Memory &memory) { return static_cast<ID3D12Heap *>(memory.internal.heap); }
inline ID3D12Resource *native(const Buffer &buffer) { return static_cast<ID3D12Resource *>(buffer.internal.resource); }
inline ID3D12GraphicsCommandList7 *native(const CommandList &cmd) {
    return static_cast<ID3D12GraphicsCommandList7 *>(cmd.internal.list);
}

// The native resource and the layout it rests in (L1). Native code that leaves
// a texture in another layout must return it to this one (P27).
struct NativeTexture {
    ID3D12Resource *resource = nullptr;
    D3D12_BARRIER_LAYOUT restingLayout = D3D12_BARRIER_LAYOUT_UNDEFINED;
};
inline NativeTexture native(const Texture &texture) {
    return {static_cast<ID3D12Resource *>(texture.internal.resource),
            static_cast<D3D12_BARRIER_LAYOUT>(texture.internal.restingLayout)};
}
inline ID3D12QueryHeap *native(const QueryPool &pool) { return static_cast<ID3D12QueryHeap *>(pool.internal.heap); }

struct NativeDescriptorHeap {
    ID3D12DescriptorHeap *resourceHeap = nullptr; // CBV_SRV_UAV, shader-visible
    ID3D12DescriptorHeap *samplerHeap = nullptr;  // SAMPLER, shader-visible
};
inline NativeDescriptorHeap native(const DescriptorHeap &heap) {
    return {static_cast<ID3D12DescriptorHeap *>(heap.internal.resourceHeap),
            static_cast<ID3D12DescriptorHeap *>(heap.internal.samplerHeap)};
}

struct NativePipeline {
    ID3D12PipelineState *pipelineState = nullptr;
    ID3D12RootSignature *rootSignature = nullptr;
};
inline NativePipeline native(const Pipeline &pipeline) {
    return {static_cast<ID3D12PipelineState *>(pipeline.internal.pipelineState),
            static_cast<ID3D12RootSignature *>(pipeline.internal.rootSignature)};
}

// Native additions at device creation (P60 (3)). D3D12 has no feature enable
// step, so the addition is a hook that runs just before D3D12CreateDevice,
// e.g. to call D3D12EnableExperimentalFeatures. listGpus also creates
// short-lived devices; the hook does not run for those.
struct NativeDeviceAdditions {
    void (*beforeDeviceCreation)(IDXGIAdapter1 *adapter, void *user) = nullptr;
    void *user = nullptr;
};

Device createDevice(const DeviceDesc &desc, const NativeDeviceAdditions &additions);

// ── Native sections (P27, P70) ────────────────────────────────────────────────

// Native commands in the middle of an RI command list. Textures are in their
// resting layouts (native(texture).restingLayout) on entry.
struct NativeCommands {
    ID3D12GraphicsCommandList7 *list = nullptr;
};
NativeCommands beginNative(CommandList &cmd);

// A texture native code left in another layout than its resting one (P27).
struct NativeLayoutChange {
    TextureView view;
    D3D12_BARRIER_LAYOUT layout = D3D12_BARRIER_LAYOUT_UNDEFINED;
};
// Ends the native section: switches the listed textures back to rest (full
// barriers on both halves) and restores everything RI set (P70): descriptor
// heaps, root signatures, the last pipelines and root data, the topology, and
// inside a rendering section the viewport and scissor. Inside a rendering
// section the list must be empty, and native code must not change attachments.
void endNative(CommandList &cmd, std::span<const NativeLayoutChange> changed = {});

// ── Import (P77) ──────────────────────────────────────────────────────────────
// Natively created objects as RI objects. RI holds no reference and releases
// nothing: destroying the RI object only forgets it (RI's own descriptors for
// an imported texture are freed). Keep the native object alive meanwhile.

// `mapped`: a CPU address for the whole heap, or nullptr (then RI buffers
// placed in it have no CPU address).
Memory importMemory(Device &device, ID3D12Heap *heap, MemoryKind kind, uint64 size, void *mapped);
Buffer importBuffer(Device &device, ID3D12Resource *buffer, const BufferDesc &desc, void *mapped);
// A 2D texture matching `desc`, with the resource flags RI gives `desc.usage`
// (ALLOW_RENDER_TARGET, ALLOW_DEPTH_STENCIL, ALLOW_UNORDERED_ACCESS). Before RI
// first uses it, bring it into native(texture).restingLayout yourself.
Texture importTexture(Device &device, ID3D12Resource *texture, const TextureDesc &desc);
// A pipeline whose root signature is RI's kind: root parameter 0 the root
// constants (b0, space999) of rootDataSize bytes, heaps directly indexed.
Pipeline importPipeline(Device &device, ID3D12PipelineState *pipeline, ID3D12RootSignature *rootSignature,
                        bool graphics, uint32 rootDataSize,
                        D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

// Copies a native descriptor from a CPU (non-shader-visible) heap into heap
// slot `slot` (P77 (2)), for view types RI does not model.
void writeNativeDescriptor(Device &device, DescriptorHeap &heap, uint32 slot, D3D12_CPU_DESCRIPTOR_HANDLE source);

} // namespace RI::D3D12
