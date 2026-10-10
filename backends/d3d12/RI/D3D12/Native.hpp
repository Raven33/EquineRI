#pragma once

// Native D3D12 access (P84). Including this header and naming the backend
// (`RI::D3D12::native(device)`) is the only way to reach D3D12 objects.

#include <RI/D3D12/Backend.hpp>

#include <d3d12.h>
#include <dxgi1_6.h>

namespace RI::D3D12 {

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

} // namespace RI::D3D12
