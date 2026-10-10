#include <TestHarness.hpp>

#include <RI/RI.hpp>
#include <RI/D3D12/Native.hpp>

#include <array>
#include <cstring>
#include <csignal>
#include <cstdlib>
#include <string>

EQ_TEST(d3d12, list_gpus) {
    const std::vector<RI::Gpu> gpus = RI::listGpus();
    EQ_CHECK(!gpus.empty());
    for (const RI::Gpu &gpu : gpus) {
        EQ_CHECK(!gpu.name.empty());
        EQ_CHECK(gpu.vendorId != 0);
        EQ_CHECK(gpu.memory > 0);
        EQ_CHECK((gpu.id != std::array<uint8, 16>{}));
    }
}

EQ_TEST(d3d12, create_destroy_device) {
    const std::vector<RI::Gpu> gpus = RI::listGpus();
    RI::Device device = RI::createDevice({.gpu = gpus[0]});
    EQ_CHECK(device.gpu.id == gpus[0].id);
    EQ_CHECK(device.gpu.name == gpus[0].name);

    const RI::D3D12::NativeDevice n = RI::D3D12::native(device);
    EQ_CHECK(n.factory != nullptr);
    EQ_CHECK(n.adapter != nullptr);
    EQ_CHECK(n.device != nullptr);
    EQ_CHECK(n.graphicsQueue != nullptr);
    EQ_CHECK(n.featureLevel >= D3D_FEATURE_LEVEL_12_0);
    EQ_CHECK(n.device->GetDeviceRemovedReason() == S_OK);

    RI::destroyDevice(device);
    EQ_CHECK(device.internal.state == nullptr);
}

// The runtime came from the Agility SDK next to the executable (P55), not the OS.
EQ_TEST(d3d12, agility_sdk_loaded) {
    RI::Device device = RI::createDevice({.gpu = RI::listGpus()[0]});
    const HMODULE core = GetModuleHandleW(L"D3D12Core.dll");
    EQ_CHECK(core != nullptr);
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(core, path, MAX_PATH);
    const std::wstring p(path);
    EQ_CHECK(p.find(L"\\D3D12\\D3D12Core.dll") != std::wstring::npos);
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
    EQ_CHECK(SUCCEEDED(RI::D3D12::native(device).device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12,
                                                                             sizeof(options12))));
    EQ_CHECK(options12.EnhancedBarriersSupported);
    RI::destroyDevice(device);
}

// Two devices at once, each with its own factory (P92). D3D12 hands out one
// ID3D12Device per adapter, so both may share it; destroying one must not
// break the other.
EQ_TEST(d3d12, two_devices) {
    const std::vector<RI::Gpu> gpus = RI::listGpus();
    RI::Device a = RI::createDevice({.gpu = gpus[0]});
    RI::Device b = RI::createDevice({.gpu = gpus[0]});
    EQ_CHECK(RI::D3D12::native(a).factory != RI::D3D12::native(b).factory);
    RI::destroyDevice(a);
    EQ_CHECK(RI::D3D12::native(b).device->GetDeviceRemovedReason() == S_OK);
    RI::destroyDevice(b);
}

// The native hook runs before device creation, with the chosen adapter (P60 (3)).
EQ_TEST(d3d12, native_hook_runs_before_creation) {
    const std::vector<RI::Gpu> gpus = RI::listGpus();
    struct Seen {
        bool called = false;
        LUID luid{};
    } seen;
    RI::Device device = RI::D3D12::createDevice(
        {.gpu = gpus[0]}, {.beforeDeviceCreation =
                               [](IDXGIAdapter1 *adapter, void *user) {
                                   auto *s = static_cast<Seen *>(user);
                                   DXGI_ADAPTER_DESC1 desc{};
                                   adapter->GetDesc1(&desc);
                                   s->called = true;
                                   s->luid = desc.AdapterLuid;
                               },
                           .user = &seen});
    EQ_CHECK(seen.called);
    EQ_CHECK(std::memcmp(&seen.luid, gpus[0].id.data(), sizeof(LUID)) == 0);
    RI::destroyDevice(device);
}

// Listing with a device alive (listing creates short-lived devices on the
// same adapters, which D3D12 shares with the live one).
EQ_TEST(d3d12, list_gpus_while_device_alive) {
    RI::Device device = RI::createDevice({.gpu = RI::listGpus()[0]});
    EQ_CHECK(!RI::listGpus().empty());
    EQ_CHECK(RI::D3D12::native(device).device->GetDeviceRemovedReason() == S_OK);
    RI::destroyDevice(device);
    EQ_CHECK(device.internal.state == nullptr);
}

// Aborts by design (see tests/CMakeLists.txt).
EQ_TEST(d3d12, unknown_gpu_reports) {
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
    RI::Gpu missing;
    missing.name = "Nonexistent GPU";
    RI::createDevice({.gpu = missing});
}
