#pragma once

// The Vulkan backend's portable surface (P83). No native types here (P84):
// native access goes through <RI/Vk/Native.hpp>.

#include <RI/Device.hpp>

#include <vector>

namespace RI::Vk {

struct DeviceState; // backend-private: native handles, function table, tracking

// A thin value (P81): copies refer to the same device. The device owns its
// Vulkan instance (P92).
struct Device {
    Gpu gpu;
    struct {
        DeviceState *state = nullptr;
    } internal;
};

// The GPUs that can run RI (Vulkan 1.3 and a graphics + compute queue), in the
// driver's order. Picking one automatically is a helper (P60).
std::vector<Gpu> listGpus();

Device createDevice(const DeviceDesc &desc);
void destroyDevice(Device &device);

} // namespace RI::Vk
