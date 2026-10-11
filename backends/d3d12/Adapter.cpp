// DXGI adapters and GPU listing on D3D12 (P60, P92).
//
// Owns: listGpus (each adapter checked on a short-lived device: feature level
// 12_0, shader model 6.6, resource binding tier 3, enhanced barriers), the
// per-device factory, and the portable Gpu description (the id is the
// adapter LUID; UMA marks integrated GPUs). Prints a warning when the
// executable has not set up the Agility SDK (exports, deployed runtime).
//
// Vulkan counterpart: backends/vulkan/Instance.cpp.

#include "Adapter.hpp"

#include "D3D12Check.hpp"

#include <RI/D3D12/Backend.hpp>
#include <RI/Error.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <string>

namespace RI::D3D12 {

namespace {

std::string toUtf8(const wchar_t *text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    return out;
}

// The feature checks behind "can run RI", on a short-lived device.
bool meetsRequirements(ID3D12Device *device, bool &uma) {
    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{.HighestShaderModel = D3D_SHADER_MODEL_6_6};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel))) ||
        shaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_6) {
        return false;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))) ||
        options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3 ||
        options.ResourceHeapTier < D3D12_RESOURCE_HEAP_TIER_2) { // any buffer in any heap (P31)
        return false;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12, sizeof(options12))) ||
        !options12.EnhancedBarriersSupported) {
        return false;
    }
    D3D12_FEATURE_DATA_ARCHITECTURE1 architecture{.NodeIndex = 0};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &architecture, sizeof(architecture)))) {
        return false;
    }
    uma = architecture.UMA != FALSE;
    return true;
}

// D3D12 loads RI's Agility SDK only when the executable exports
// D3D12SDKVersion and the runtime sits in <exe dir>/D3D12/ (P55). Without
// them it falls back to the system runtime, which may reject every adapter.
// Returns what is missing, or nothing.
std::string agilityProblem() {
    HMODULE exe = GetModuleHandleW(nullptr);
    if (GetProcAddress(exe, "D3D12SDKVersion") == nullptr) {
        return "the executable does not export D3D12SDKVersion: invoke EQUINE_D3D12_AGILITY_SDK_EXPORTS() in one "
               ".cpp of the executable (D3D12AgilityExports.hpp)";
    }
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(exe, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    const std::filesystem::path core = std::filesystem::path(path).parent_path() / "D3D12" / "D3D12Core.dll";
    if (!std::filesystem::exists(core)) {
        return "D3D12Core.dll is not in the executable's D3D12 folder: call equine_deploy_agility_sdk(<target>) in "
               "CMake";
    }
    return {};
}

} // namespace

IDXGIFactory6 *createFactory() {
    // No DXGI_CREATE_FACTORY_DEBUG: it needs Windows' optional Graphics Tools,
    // while the D3D12 debug layer ships with the Agility SDK.
    IDXGIFactory6 *factory = nullptr;
    checkHr(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
    return factory;
}

bool describeGpu(IDXGIAdapter1 *adapter, Gpu &gpu) {
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc))) {
        return false;
    }
    ID3D12Device *device = nullptr;
    if (FAILED(D3D12CreateDevice(adapter, kMinFeatureLevel, IID_PPV_ARGS(&device)))) {
        return false;
    }
    bool uma = false;
    const bool usable = meetsRequirements(device, uma);
    release(device);
    if (!usable) {
        return false;
    }

    gpu.name = toUtf8(desc.Description);
    gpu.vendorId = desc.VendorId;
    gpu.deviceId = desc.DeviceId;
    switch (desc.VendorId) {
    case 0x10DE: gpu.vendor = GpuVendor::eNvidia; break;
    case 0x1002:
    case 0x1022: gpu.vendor = GpuVendor::eAMD; break;
    case 0x8086: gpu.vendor = GpuVendor::eIntel; break;
    default: gpu.vendor = GpuVendor::eOther; break;
    }
    if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
        gpu.kind = GpuKind::eSoftware;
    } else {
        gpu.kind = uma ? GpuKind::eIntegrated : GpuKind::eDiscrete;
    }
    // Dedicated video memory; on UMA GPUs (little or none dedicated), the
    // system memory the GPU may use, as Vulkan's largest device-local heap.
    gpu.memory = uma ? std::max<uint64>(desc.DedicatedVideoMemory, desc.SharedSystemMemory) : desc.DedicatedVideoMemory;

    gpu.id = {};
    static_assert(sizeof(desc.AdapterLuid) <= sizeof(gpu.id));
    std::memcpy(gpu.id.data(), &desc.AdapterLuid, sizeof(desc.AdapterLuid));
    return true;
}

IDXGIAdapter1 *findGpu(IDXGIFactory6 *factory, const Gpu &gpu, Gpu &found) {
    LUID luid{};
    std::memcpy(&luid, gpu.id.data(), sizeof(luid));
    IDXGIAdapter1 *adapter = nullptr;
    if (SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) && describeGpu(adapter, found) &&
        found.id == gpu.id) {
        return adapter;
    }
    release(adapter);
    reportError(ErrorCategory::eProgrammerError,
                std::format("the GPU '{}' is not available; pass a GPU returned by listGpus()", gpu.name));
}

std::vector<Gpu> listGpus() {
    IDXGIFactory6 *factory = createFactory();
    std::vector<Gpu> gpus;
    IDXGIAdapter1 *adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        Gpu gpu;
        if (describeGpu(adapter, gpu)) {
            gpus.push_back(std::move(gpu));
        }
        release(adapter);
    }
    release(factory);
    // A missing Agility SDK is a setup mistake, not "no GPU": say so, once.
    static bool reported = false;
    if (const std::string problem = agilityProblem(); !problem.empty() && !reported) {
        reported = true;
        std::fprintf(stderr, "[EquineRI] Agility SDK not loaded (%zu GPU(s) listed): %s\n", gpus.size(),
                     problem.c_str());
    }
    return gpus;
}

} // namespace RI::D3D12
