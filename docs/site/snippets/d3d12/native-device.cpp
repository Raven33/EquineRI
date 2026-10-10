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

    // --8<-- [start:object-handles]
    RI::Memory memory = RI::allocateMemory(device, RI::MemoryKind::eGPU, 65536);
    RI::Buffer buffer = RI::createBuffer(device, memory, 0, {.size = 1024, .usage = RI::BufferUsage::eReadWrite});
    RI::DescriptorHeap heap = RI::createDescriptorHeap(device, {.resourceCount = 16, .samplerCount = 16});
    ID3D12Resource *resource = RI::D3D12::native(buffer); // borrowed: no Release
    ID3D12Heap *nativeMemory = RI::D3D12::native(memory);
    const RI::D3D12::NativeDescriptorHeap nativeHeap = RI::D3D12::native(heap);
    // Name the buffer for PIX and debug-layer messages.
    resource->SetName(L"particles");
    std::printf("heap %llu bytes; view heap holds %u descriptors\n",
                static_cast<unsigned long long>(nativeMemory->GetDesc().SizeInBytes),
                nativeHeap.resourceHeap->GetDesc().NumDescriptors);
    // --8<-- [end:object-handles]

    // --8<-- [start:native-command]
    RI::CommandList cmd = RI::createCommandList(device);
    RI::cmdBegin(cmd);
    // A native command recorded into RI's command list: write one 32-bit value.
    // It changes no state RI relies on (no heaps, root signature or pipeline).
    ID3D12GraphicsCommandList7 *list = RI::D3D12::native(cmd);
    const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER write{.Dest = buffer.gpuAddress, .Value = 42};
    list->WriteBufferImmediate(1, &write, nullptr);
    RI::cmdEnd(cmd);
    RI::wait(device.graphicsQueue, RI::submit(device.graphicsQueue, cmd));
    // --8<-- [end:native-command]

    // --8<-- [start:native-texture]
    // A texture's resource and the layout it rests in. Native code may move it
    // to another layout, but must return it to restingLayout before RI uses it.
    RI::Memory textureMemory = RI::allocateMemory(device, RI::MemoryKind::eGPU, 16u << 20);
    RI::Texture texture = RI::createTexture(device, textureMemory, 0,
                                            {.width = 256,
                                             .height = 256,
                                             .format = RI::Format::eRGBA8Unorm,
                                             .usage = RI::TextureUsage::eSampled | RI::TextureUsage::eCopyDst});
    const RI::D3D12::NativeTexture native = RI::D3D12::native(texture); // borrowed: no Release
    native.resource->SetName(L"albedo");
    const bool common = native.restingLayout == D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COMMON; // sampled or copied
    std::printf("rests in DIRECT_QUEUE_COMMON: %s\n", common ? "yes" : "no");
    // --8<-- [end:native-texture]

    // --8<-- [start:native-query-pool]
    RI::QueryPool pool = RI::createQueryPool(device, {.type = RI::QueryType::eTimestamp, .count = 2});
    ID3D12QueryHeap *queryHeap = RI::D3D12::native(pool); // a TIMESTAMP query heap
    queryHeap->SetName(L"frame timestamps");
    // --8<-- [end:native-query-pool]

    RI::destroyQueryPool(device, pool);
    RI::destroyTexture(device, texture);
    RI::freeMemory(device, textureMemory);
    RI::destroyCommandList(device, cmd);
    RI::destroyDescriptorHeap(device, heap);
    RI::destroyBuffer(device, buffer);
    RI::freeMemory(device, memory);

    std::printf("experimental shader models: %s\n", experimental ? "on" : "off");
    RI::destroyDevice(device);
    return 0;
}
