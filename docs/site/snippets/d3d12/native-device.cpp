// RI with D3D12 native access (P84, P92): a hook that runs just before
// D3D12CreateDevice, and the native handles of an RI device.
// Compiled against EquineD3D12Backend only (Windows).

#include <RI/RI.hpp>
#include <RI/D3D12/Native.hpp>

#include <D3D12AgilityExports.hpp>

#include <cstdio>
#include <vector>

// --8<-- [start:agility-exports]
// Once per executable, in one .cpp file (P55). CMake: equine_deploy_agility_sdk(<target>).
EQUINE_D3D12_AGILITY_SDK_EXPORTS()
// --8<-- [end:agility-exports]

// --8<-- [start:hook]
// Runs on the adapter RI picked, just before RI calls D3D12CreateDevice.
void enableExperimentalShaderModels(IDXGIAdapter1 * /*adapter*/, void *user) {
    UUID features[] = {D3D12ExperimentalShaderModels};
    const HRESULT result = D3D12EnableExperimentalFeatures(1, features, nullptr, nullptr);
    *static_cast<bool *>(user) = SUCCEEDED(result); // fails without Windows developer mode
}
// --8<-- [end:hook]

int main() {
    const std::vector<RI::Gpu> gpus = RI::listGpus();
    if (gpus.empty()) {
        return 1;
    }

    // --8<-- [start:create-with-hook]
    bool experimental = false;
    RI::Device device = RI::D3D12::createDevice(
        {.gpu = gpus[0]}, {.beforeDeviceCreation = &enableExperimentalShaderModels, .user = &experimental});
    // --8<-- [end:create-with-hook]

    // --8<-- [start:native-handles]
    // Borrowed pointers: RI keeps its references. AddRef to keep one longer.
    const RI::D3D12::NativeDevice n = RI::D3D12::native(device);
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
    n.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7));
    std::printf("mesh shaders: %s\n", options7.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED ? "yes" : "no");
    // --8<-- [end:native-handles]

    std::printf("experimental shader models: %s\n", experimental ? "on" : "off");
    RI::destroyDevice(device);
    return 0;
}
