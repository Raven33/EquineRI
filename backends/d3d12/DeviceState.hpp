#pragma once

// Backend-private state behind RI::D3D12::Device (P82: hidden state behind
// `internal`). Heap-allocated once per device, so Device stays a small value.

#include <RI/D3D12/Backend.hpp>
#if EQUINE_VALIDATION
#include <RI/Validation/ObjectTracker.hpp>
#include <RI/Validation/Placement.hpp>
#endif

#include <string_view>
#include <vector>

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
    uint32 depthResolveModes = 0; // DeviceLimits copies (P76), for checks while recording
    uint32 stencilResolveModes = 0;

    // The graphics queue's timeline (P20).
    ID3D12Fence *fence = nullptr;
    uint64 lastSignaled = 0;

    // RI's own CPU descriptor heaps for render-target and depth-stencil views
    // (created explicitly with views, P73), with free lists of slots.
    struct CpuHeap {
        ID3D12DescriptorHeap *heap = nullptr;
        UINT increment = 0;
        std::vector<uint32> free;
    };
    CpuHeap rtvHeap;
    CpuHeap dsvHeap;

    // RI's own small shader-visible heap (and its CPU twin) for UAV clears
    // (P65): the user's heap holds no RI slots.
    ID3D12DescriptorHeap *clearHeap = nullptr;
    ID3D12DescriptorHeap *clearCpuHeap = nullptr;
    uint32 clearNext = 0;

    // Internal commands (texture resets at creation, R16), submitted at once.
    ID3D12CommandAllocator *internalAllocator = nullptr;
    ID3D12GraphicsCommandList7 *internalList = nullptr;

#if EQUINE_VALIDATION
    Validation::ObjectTracker objects;
    Validation::PlacementTracker placements;
#endif
};

// Debugging (Debug.cpp, P53): a removed device is reported with DRED's
// breadcrumbs and page-fault details (validation builds).
void reportDeviceRemoved(DeviceState &state);
// checkHr for queue submission and waits.
void checkQueueHr(DeviceState &state, HRESULT result, std::string_view call);

inline constexpr uint32 kRtvHeapSize = 4096;
inline constexpr uint32 kDsvHeapSize = 1024;
inline constexpr uint32 kClearHeapSize = 1024;

// A slot in one of RI's CPU heaps; reports a readable error when it is full.
uint32 allocateSlot(DeviceState::CpuHeap &heap, const char *what);
D3D12_CPU_DESCRIPTOR_HANDLE slotHandle(const DeviceState::CpuHeap &heap, uint32 slot);

// Records `record(list)` on the internal list, submits it and waits (R16).
template <typename F>
void submitInternal(DeviceState &state, F record);
void beginInternal(DeviceState &state);
void endAndWaitInternal(DeviceState &state);
template <typename F>
void submitInternal(DeviceState &state, F record) {
    beginInternal(state);
    record(state.internalList);
    endAndWaitInternal(state);
}

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
