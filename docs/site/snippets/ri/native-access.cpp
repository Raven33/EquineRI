// RI: native access from code that builds on both backends (P84). Each native
// part sits in an #if block; this file is compiled once per enabled backend.

// --8<-- [start:includes]
#include <RI/RI.hpp>
#if EQUINE_BACKEND_VULKAN
#include <RI/Vk/Native.hpp>
#elif EQUINE_BACKEND_D3D12
#include <RI/D3D12/Native.hpp>
#endif
// --8<-- [end:includes]

#include <cstdio>

// --8<-- [start:per-backend]
void printNativeDevice(const RI::Device &device) {
#if EQUINE_BACKEND_VULKAN
    const VkDevice native = RI::Vk::native(device).device;
#elif EQUINE_BACKEND_D3D12
    const ID3D12Device *native = RI::D3D12::native(device).device;
#endif
    std::printf("native device: %p\n", static_cast<const void *>(native));
}
// --8<-- [end:per-backend]

int main() {
    RI::Device device = RI::createDevice({.gpu = RI::listGpus().at(0)});
    printNativeDevice(device);
    RI::destroyDevice(device);
    return 0;
}
