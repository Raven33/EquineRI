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
