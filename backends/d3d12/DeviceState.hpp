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
    ID3D12Device10 *device = nullptr; // enhanced barriers, CreatePlacedResource2
    ID3D12CommandQueue *graphicsQueue = nullptr;
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_12_0;
    ID3D12InfoQueue1 *infoQueue = nullptr; // validation builds only
    DWORD messageCallbackCookie = 0;

    UINT resourceDescriptorSize = 0;
    UINT samplerDescriptorSize = 0;
    bool gpuUploadHeap = false;

    // The graphics queue's timeline (P20).
    ID3D12Fence *fence = nullptr;
    uint64 lastSignaled = 0;

#if EQUINE_VALIDATION
    Validation::ObjectTracker objects;
#endif
};

// Validation builds: the id of a new object, and the checks on its use.
#if EQUINE_VALIDATION
#define EQ_DX_TRACK(state) ::RI::Validation::trackObject((state)->objects)
#define EQ_DX_UNTRACK(state, id) ::RI::Validation::untrackObject((state)->objects, id)
#define EQ_DX_CHECK_ALIVE(state, id, kind) EQ_VALIDATE(::RI::Validation::checkAlive((state)->objects, id, kind))
#else
#define EQ_DX_TRACK(state) uint64{0}
#define EQ_DX_UNTRACK(state, id) ((void)0)
#define EQ_DX_CHECK_ALIVE(state, id, kind) ((void)0)
#endif

} // namespace RI::D3D12
