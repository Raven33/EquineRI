#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
using Microsoft::WRL::ComPtr;

int main() {
    ComPtr<IDXGIFactory6> f;
    CreateDXGIFactory1(IID_PPV_ARGS(&f));
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; f->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_UNSPECIFIED, IID_PPV_ARGS(&a)) == S_OK; ++i) {
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        ComPtr<ID3D12Device> dev;
        HRESULT hr = D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));
        int tier = -1;
        if (SUCCEEDED(hr)) {
            D3D12_FEATURE_DATA_D3D12_OPTIONS21 o{};
            if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS21, &o, sizeof o))) tier = (int)o.WorkGraphsTier;
        }
        wprintf(L"%u %s createDevice=0x%08x WorkGraphsTier=%d\n", i, d.Description, (unsigned)hr, tier);
        a.Reset();
    }
}
