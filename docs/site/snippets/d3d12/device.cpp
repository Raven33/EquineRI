// Canonical D3D12: export the Agility SDK version, create a DXGI factory, list
// the GPUs that can run shader model 6.6 with bindless and enhanced barriers,
// create a device and its DIRECT queue, release everything.
//
// A stand-alone program: it uses only D3D12 and DXGI (no RI). It is not what
// RI's backend does line by line; it is how you would do the same thing in
// your own D3D12 renderer. Compiled against EquineD3D12Backend for the pinned
// Agility SDK headers (Windows only).

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>

using Microsoft::WRL::ComPtr;

// --8<-- [start:agility-exports]
// The Agility SDK: D3D12 loads the newer runtime shipped next to your program
// (in .\D3D12\) only if the *executable* exports these two symbols.
extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = D3D12_SDK_VERSION;
__declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";
}
// --8<-- [end:agility-exports]

int main() {
    // --8<-- [start:debug-layer]
#ifndef NDEBUG
    // Debug builds: the debug layer must be on before the device is created.
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
    }
#endif
    // --8<-- [end:debug-layer]

    // --8<-- [start:create-factory]
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        return 1;
    }
    // --8<-- [end:create-factory]

    // --8<-- [start:enumerate-gpus]
    ComPtr<IDXGIAdapter1> chosen;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        // --8<-- [start:describe-gpu]
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        // desc.Description: the name (UTF-16). desc.VendorId / DeviceId: PCI IDs.
        // desc.DedicatedVideoMemory: VRAM. desc.AdapterLuid: a stable ID until reboot.
        // (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0: WARP, a CPU implementation.

        // D3D12 has no "list features" call on an adapter: create a short-lived
        // device and ask it.
        ComPtr<ID3D12Device> probe;
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&probe)))) {
            continue;
        }
        D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{.HighestShaderModel = D3D_SHADER_MODEL_6_6};
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
        const bool usable =
            SUCCEEDED(probe->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel))) &&
            shaderModel.HighestShaderModel >= D3D_SHADER_MODEL_6_6 &&
            SUCCEEDED(probe->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))) &&
            options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3 &&
            SUCCEEDED(probe->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12, sizeof(options12))) &&
            options12.EnhancedBarriersSupported;
        std::wprintf(L"%ls (%llu MiB)%ls\n", desc.Description,
                     static_cast<unsigned long long>(desc.DedicatedVideoMemory >> 20),
                     usable ? L"" : L" - cannot run this program");
        // --8<-- [end:describe-gpu]
        if (usable && chosen == nullptr) {
            chosen = adapter;
        }
    }
    // --8<-- [end:enumerate-gpus]
    if (chosen == nullptr) {
        return 1;
    }

    // --8<-- [start:experimental-features]
    // Optional. Experimental features (preview shader models, ...) are switched
    // on for the whole process, before D3D12CreateDevice. Fails without
    // Windows developer mode.
    constexpr bool wantExperimentalShaderModels = false;
    if (wantExperimentalShaderModels) {
        UUID experimental[] = {D3D12ExperimentalShaderModels};
        if (FAILED(D3D12EnableExperimentalFeatures(1, experimental, nullptr, nullptr))) {
            std::printf("experimental shader models unavailable\n");
        }
    }
    // --8<-- [end:experimental-features]

    // --8<-- [start:create-device]
    // No feature list: everything the GPU supports is available once the device exists.
    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(chosen.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
        return 1;
    }
    // A DIRECT queue accepts graphics, compute and copy work.
    const D3D12_COMMAND_QUEUE_DESC queueDesc{.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)))) {
        return 1;
    }
    // --8<-- [end:create-device]

    // --8<-- [start:destroy-device]
    // COM objects are reference counted: Reset() (or leaving the scope) releases
    // them. With work in flight, wait on a fence first.
    queue.Reset();
    device.Reset();
    factory.Reset();
    // --8<-- [end:destroy-device]
    return 0;
}
