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
// finds the same GPU again through `id`. Lists follow the native API, so they
// can differ between backends: D3D12 also lists WARP (eSoftware), and DXGI may
// list one physical GPU twice under two LUIDs (seen with an RTX 3080 on Windows).
struct Gpu {
    std::string name;
    GpuVendor vendor = GpuVendor::eOther;
    GpuKind kind = GpuKind::eOther;
    uint32 vendorId = 0; // PCI vendor ID
    uint32 deviceId = 0; // PCI device ID
    // Bytes of GPU memory. Discrete GPUs: the VRAM. Integrated GPUs: the system
    // memory the GPU may use, as each API reports it (Vulkan: the largest
    // device-local heap; D3D12: shared system memory), so the backends differ.
    uint64 memory = 0;
    // Stable within the process: Vulkan deviceUUID, D3D12 adapter LUID (first 8 bytes).
    std::array<uint8, 16> id{};
};

struct DeviceDesc {
    Gpu gpu;
};

// Limits RI reports for the device (P59, P44).
struct DeviceLimits {
    uint32 maxResourceDescriptors = 0; // per descriptor heap
    uint32 maxSamplerDescriptors = 0;  // per descriptor heap
    uint32 maxRootDataSize = 0;        // bytes of root data a pipeline may declare
    bool gpuUploadMemory = false;      // MemoryKind::eGPUUpload is available (P32)
    uint64 timestampFrequency = 0;     // timestamp ticks per second (P71 (3))
};

} // namespace RI
