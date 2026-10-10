#pragma once

// The D3D12 backend's portable surface (P83). No native types here (P84):
// native access goes through <RI/D3D12/Native.hpp>.

#include <RI/Device.hpp>

#include <vector>

namespace RI::D3D12 {

struct DeviceState; // backend-private: native objects, tracking

// A thin value (P81): copies refer to the same device. The device owns its
// DXGI factory (P92).
struct Device {
    Gpu gpu;
    struct {
        DeviceState *state = nullptr;
    } internal;
};

// The GPUs that can run RI (feature level 12_0, shader model 6.6, resource
// binding tier 3, enhanced barriers), in DXGI's order. Checking them creates a
// short-lived device per adapter. Picking one automatically is a helper (P60).
std::vector<Gpu> listGpus();

Device createDevice(const DeviceDesc &desc);
void destroyDevice(Device &device);

} // namespace RI::D3D12
