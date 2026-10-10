#pragma once

// Helpers shared by the portable GPU tests (tests/gpu/).

#include <RI/RI.hpp>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace gputest {

// The GPU to test: EQUINE_GPU=<index> or nvidia/amd/intel, else the first listed.
inline RI::Gpu pickGpu() {
    const std::vector<RI::Gpu> gpus = RI::listGpus();
    std::string want;
#if defined(_WIN32)
    char *value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, "EQUINE_GPU") == 0 && value != nullptr) {
        want = value;
        std::free(value);
    }
#else
    if (const char *value = std::getenv("EQUINE_GPU"); value != nullptr) {
        want = value;
    }
#endif
    if (want.empty()) {
        return gpus[0];
    }
    for (const RI::Gpu &gpu : gpus) {
        if ((want == "nvidia" && gpu.vendor == RI::GpuVendor::eNvidia) ||
            (want == "amd" && gpu.vendor == RI::GpuVendor::eAMD) ||
            (want == "intel" && gpu.vendor == RI::GpuVendor::eIntel)) {
            return gpu;
        }
    }
    const size_t index = std::strtoul(want.c_str(), nullptr, 10);
    return gpus[index < gpus.size() ? index : 0];
}

inline RI::Device createTestDevice() { return RI::createDevice({.gpu = pickGpu()}); }

// The shader compiled for this backend: SPIR-V for Vulkan, DXIL for D3D12.
inline std::vector<std::byte> loadShader(const char *name) {
#if EQUINE_BACKEND_VULKAN
    const std::string path = std::string(EQUINE_SHADER_DIR) + "/" + name + ".spv";
#else
    const std::string path = std::string(EQUINE_SHADER_DIR) + "/" + name + ".dxil";
#endif
    std::ifstream file(path, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    std::vector<std::byte> out(bytes.size());
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return out;
}

inline uint64 alignUp(uint64 value, uint64 alignment) { return (value + alignment - 1) / alignment * alignment; }

// A linear allocator over one memory block, for tests.
struct Arena {
    RI::Memory memory;
    uint64 used = 0;
};

inline RI::Buffer placeBuffer(RI::Device &device, Arena &arena, const RI::BufferDesc &desc) {
    const RI::MemoryRequirements req = RI::bufferRequirements(device, desc);
    const uint64 offset = alignUp(arena.used, req.alignment);
    arena.used = offset + req.size;
    return RI::createBuffer(device, arena.memory, offset, desc);
}

inline RI::Texture placeTexture(RI::Device &device, Arena &arena, const RI::TextureDesc &desc) {
    const RI::MemoryRequirements req = RI::textureRequirements(device, desc);
    const uint64 offset = alignUp(arena.used, req.alignment);
    arena.used = offset + req.size;
    return RI::createTexture(device, arena.memory, offset, desc);
}

} // namespace gputest
