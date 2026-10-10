#pragma once

// Portable GPU and device descriptions (CONTEXT.md P60, P92). The device itself
// is a per-backend struct (P83): RI::Vk::Device, RI::D3D12::Device.

#include <types.hpp>

#include <array>
#include <string>

namespace RI {

enum class GpuVendor : uint8 {
    eOther,
    eNvidia,
    eAMD,
    eIntel,
};

enum class GpuKind : uint8 {
    eOther,
    eDiscrete,
    eIntegrated, // shares system memory (laptops, APUs)
    eSoftware,   // a CPU implementation (llvmpipe, WARP)
};

// One GPU as listed by listGpus(): plain facts, no native object. createDevice
// finds the same GPU again through `id`.
struct Gpu {
    std::string name;
    GpuVendor vendor = GpuVendor::eOther;
    GpuKind kind = GpuKind::eOther;
    uint32 vendorId = 0; // PCI vendor ID
    uint32 deviceId = 0; // PCI device ID
    uint64 memory = 0;   // bytes of GPU memory (the largest device-local heap)
    // Stable within the process: Vulkan deviceUUID, D3D12 adapter LUID (first 8 bytes).
    std::array<uint8, 16> id{};
};

struct DeviceDesc {
    Gpu gpu;
};

} // namespace RI
