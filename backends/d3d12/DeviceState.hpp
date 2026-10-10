#pragma once

// Backend-private state behind RI::D3D12::Device (P82: hidden state behind
// `internal`). Heap-allocated once per device, so Device stays a small value.

#include <RI/D3D12/Backend.hpp>
#if EQUINE_VALIDATION
#include <RI/Validation/ObjectTracker.hpp>
#endif

#include <d3d12.h>
#include <d3d12sdklayers.h> // ID3D12InfoQueue1
#include <dxgi1_6.h>

namespace RI::D3D12 {

struct DeviceState {
    IDXGIFactory6 *factory = nullptr;
    IDXGIAdapter1 *adapter = nullptr;
    ID3D12Device *device = nullptr;
    ID3D12CommandQueue *graphicsQueue = nullptr;
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_12_0;
    ID3D12InfoQueue1 *infoQueue = nullptr; // validation builds only
    DWORD messageCallbackCookie = 0;
#if EQUINE_VALIDATION
    Validation::ObjectTracker objects;
#endif
};

} // namespace RI::D3D12
