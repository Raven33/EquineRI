// Canonical D3D12: how errors are reported without RI. Calls return an HRESULT
// that you check yourself; debug-layer messages arrive through
// ID3D12InfoQueue1; a lost device shows up as DXGI_ERROR_DEVICE_REMOVED.
//
// A stand-alone program. Compiled against EquineD3D12Backend for the pinned
// Agility SDK headers (Windows only).

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>

using Microsoft::WRL::ComPtr;

// --8<-- [start:check-hresult]
// FAILED() is true for negative HRESULTs. After DXGI_ERROR_DEVICE_REMOVED,
// GetDeviceRemovedReason says why (hang, reset, driver error).
void check(HRESULT result, const char *call) {
    if (FAILED(result)) {
        std::fprintf(stderr, "%s failed: HRESULT 0x%08lX\n", call, static_cast<unsigned long>(result));
        std::abort();
    }
}

// The debug layer reports mistakes here, not through HRESULTs.
void CALLBACK onMessage(D3D12_MESSAGE_CATEGORY /*category*/, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID /*id*/,
                        LPCSTR description, void * /*context*/) {
    if (severity == D3D12_MESSAGE_SEVERITY_ERROR || severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
        std::fprintf(stderr, "debug layer error: %s\n", description);
    }
}
// --8<-- [end:check-hresult]

int main() {
    // --8<-- [start:info-queue]
    ComPtr<ID3D12Debug> debug;
    check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "D3D12GetDebugInterface");
    debug->EnableDebugLayer(); // before creating the device
    ComPtr<ID3D12Device> device;
    check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
    ComPtr<ID3D12InfoQueue1> infoQueue; // Agility SDK debug layer
    DWORD cookie = 0;
    if (SUCCEEDED(device.As(&infoQueue))) {
        check(infoQueue->RegisterMessageCallback(&onMessage, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie),
              "RegisterMessageCallback");
    }
    // --8<-- [end:info-queue]
    return 0;
}
