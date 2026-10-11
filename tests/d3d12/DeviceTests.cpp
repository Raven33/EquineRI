// D3D12-only GPU tests: GPU listing and device creation, the Agility SDK, the
// native creation hook, native sections and import, and work graphs. Portable
// tests live in tests/gpu/.

#include <TestHarness.hpp>

#include "../gpu/GpuTestCommon.hpp"

#include <RI/RI.hpp>
#include <RI/D3D12/Native.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <csignal>
#include <cstdlib>
#include <string>
#include <vector>

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

namespace {

struct WriteRoot {
    uint32 textureSlot;
    uint32 width;
    uint32 height;
    float value;
};

template <typename Record>
void runOnce(RI::Device &device, Record record) {
    RI::CommandList cmd = RI::createCommandList(device);
    RI::cmdBegin(cmd);
    record(cmd);
    RI::cmdEnd(cmd);
    RI::wait(device.graphicsQueue, RI::submit(device.graphicsQueue, cmd));
    RI::destroyCommandList(device, cmd);
}

void textureBarrier(ID3D12GraphicsCommandList7 *list, ID3D12Resource *resource, D3D12_BARRIER_LAYOUT before,
                    D3D12_BARRIER_LAYOUT after, D3D12_BARRIER_ACCESS accessAfter) {
    D3D12_TEXTURE_BARRIER barrier{.SyncBefore = D3D12_BARRIER_SYNC_ALL,
                                  .SyncAfter = D3D12_BARRIER_SYNC_ALL,
                                  .AccessBefore = before == D3D12_BARRIER_LAYOUT_UNDEFINED
                                                      ? D3D12_BARRIER_ACCESS_NO_ACCESS
                                                      : D3D12_BARRIER_ACCESS_COMMON,
                                  .AccessAfter = accessAfter,
                                  .LayoutBefore = before,
                                  .LayoutAfter = after,
                                  .pResource = resource,
                                  .Subresources = {.IndexOrFirstMipLevel = 0xffffffff}};
    const D3D12_BARRIER_GROUP group{.Type = D3D12_BARRIER_TYPE_TEXTURE, .NumBarriers = 1, .pTextureBarriers = &barrier};
    list->Barrier(1, &group);
}

} // namespace

// Native sections (P27, P70): native code fills a texture with a copy and
// leaves it in COPY_DEST (declared on exit), and disturbs the root constants;
// after endNative RI's dispatch uses RI's state again. The storage descriptor
// is written natively (P77 (2)).
EQ_TEST(d3d12, native_section) {
    RI::Device device = RI::createDevice({.gpu = gputest::pickGpu()});
    ID3D12Device *nd = RI::D3D12::native(device).device;
    gputest::Arena gpu{.memory = RI::allocateMemory(device, RI::MemoryKind::eGPU, 16 << 20)};
    gputest::Arena upload{.memory = RI::allocateMemory(device, RI::MemoryKind::eUpload, 1 << 20)};
    gputest::Arena readback{.memory = RI::allocateMemory(device, RI::MemoryKind::eReadback, 1 << 20)};
    RI::DescriptorHeap heap = RI::createDescriptorHeap(device, {.resourceCount = 8});
    RI::Texture storage = gputest::placeTexture(device, gpu, {.width = 16, .height = 16, .format = RI::Format::eRGBA32Float,
                                                              .usage = RI::TextureUsage::eStorage | RI::TextureUsage::eCopySrc});
    RI::Texture filled = gputest::placeTexture(device, gpu, {.width = 16, .height = 16, .format = RI::Format::eRGBA8Unorm,
                                                             .usage = RI::TextureUsage::eSampled | RI::TextureUsage::eCopySrc |
                                                                      RI::TextureUsage::eCopyDst});
    // The storage UAV, created in a native CPU heap and copied into slot 2.
    ID3D12DescriptorHeap *cpuHeap = nullptr;
    const D3D12_DESCRIPTOR_HEAP_DESC cpuDesc{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, .NumDescriptors = 1};
    EQ_CHECK(SUCCEEDED(nd->CreateDescriptorHeap(&cpuDesc, IID_PPV_ARGS(&cpuHeap))));
    const D3D12_UNORDERED_ACCESS_VIEW_DESC uav{.Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
                                               .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D};
    nd->CreateUnorderedAccessView(RI::D3D12::native(storage).resource, nullptr, &uav,
                                  cpuHeap->GetCPUDescriptorHandleForHeapStart());
    RI::D3D12::writeNativeDescriptor(device, heap, 2, cpuHeap->GetCPUDescriptorHandleForHeapStart());
    const std::vector<std::byte> shader = gputest::loadShader("write_storage");
    RI::Pipeline write = RI::createComputePipeline(device, {.shader = shader});
    const RI::CopyLayout storageLayout = RI::copyLayout(device, storage.view);
    const RI::CopyLayout filledLayout = RI::copyLayout(device, filled.view);
    RI::Buffer source = gputest::placeBuffer(device, upload, {.size = filledLayout.size, .usage = RI::BufferUsage::eCopySrc});
    std::memset(source.mapped, 0x80, filledLayout.size);
    RI::Buffer storageBack = gputest::placeBuffer(device, readback, {.size = storageLayout.size, .usage = RI::BufferUsage::eCopyDst});
    RI::Buffer filledBack = gputest::placeBuffer(device, readback, {.size = filledLayout.size, .usage = RI::BufferUsage::eCopyDst});

    runOnce(device, [&](RI::CommandList &cmd) {
        RI::cmdSetHeap(cmd, heap);
        RI::cmdSetPipeline(cmd, write);
        RI::cmdSetRootData(cmd, WriteRoot{.textureSlot = 2, .width = 16, .height = 16, .value = 9.0f});
        const RI::D3D12::NativeCommands n = RI::D3D12::beginNative(cmd);
        const WriteRoot wrong{.textureSlot = 2, .width = 16, .height = 16, .value = 1.0f};
        n.list->SetComputeRoot32BitConstants(0, sizeof(wrong) / 4, &wrong, 0);
        ID3D12Resource *texture = RI::D3D12::native(filled).resource;
        textureBarrier(n.list, texture, RI::D3D12::native(filled).restingLayout, D3D12_BARRIER_LAYOUT_COPY_DEST,
                       D3D12_BARRIER_ACCESS_COPY_DEST);
        const D3D12_RESOURCE_DESC desc = texture->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        nd->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, nullptr);
        D3D12_TEXTURE_COPY_LOCATION to{.pResource = texture, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        to.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION from{.pResource = RI::D3D12::native(source),
                                         .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        from.PlacedFootprint = footprint;
        n.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        const RI::D3D12::NativeLayoutChange left[] = {{filled.view, D3D12_BARRIER_LAYOUT_COPY_DEST}};
        RI::D3D12::endNative(cmd, left);
        RI::cmdDispatch(cmd, 2, 2, 1); // RI's root data again: 9
        RI::cmdBarrier(cmd, RI::Stage::eAll, RI::Stage::eCopy);
        RI::cmdCopyTextureToBuffer(cmd, storage.view, storageBack, 0);
        RI::cmdCopyTextureToBuffer(cmd, filled.view, filledBack, 0);
    });
    uint32 wrong = 0;
    for (uint32 y = 0; y < 16; ++y) {
        const auto *row = reinterpret_cast<const float *>(static_cast<const uint8 *>(storageBack.mapped) +
                                                          y * storageLayout.rowPitch);
        const auto *bytes = static_cast<const uint8 *>(filledBack.mapped) + y * filledLayout.rowPitch;
        for (uint32 x = 0; x < 16; ++x) {
            wrong += row[x * 4 + 2] != 9.0f;
            wrong += bytes[x * 4] != 0x80;
        }
    }
    EQ_CHECK(wrong == 0);

    cpuHeap->Release();
    RI::destroyBuffer(device, filledBack);
    RI::destroyBuffer(device, storageBack);
    RI::destroyBuffer(device, source);
    RI::destroyPipeline(device, write);
    RI::destroyTexture(device, filled);
    RI::destroyTexture(device, storage);
    RI::destroyDescriptorHeap(device, heap);
    RI::freeMemory(device, readback.memory);
    RI::freeMemory(device, upload.memory);
    RI::freeMemory(device, gpu.memory);
    RI::destroyDevice(device);
}

// Import (P77): a native placed texture, a native heap and a committed buffer
// used by RI; RI forgets them on destroy and the native objects stay valid.
EQ_TEST(d3d12, import_objects) {
    RI::Device device = RI::createDevice({.gpu = gputest::pickGpu()});
    ID3D12Device *nd = RI::D3D12::native(device).device;
    RI::Memory gpuMemory = RI::allocateMemory(device, RI::MemoryKind::eGPU, 4 << 20);

    const RI::TextureDesc desc{.width = 16, .height = 16, .format = RI::Format::eRGBA8Unorm,
                               .usage = RI::TextureUsage::eSampled | RI::TextureUsage::eCopySrc | RI::TextureUsage::eCopyDst};
    const D3D12_RESOURCE_DESC1 textureDesc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                           .Width = 16,
                                           .Height = 16,
                                           .DepthOrArraySize = 1,
                                           .MipLevels = 1,
                                           .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
                                           .SampleDesc = {1, 0},
                                           .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN};
    ID3D12Device10 *device10 = nullptr;
    EQ_CHECK(SUCCEEDED(nd->QueryInterface(IID_PPV_ARGS(&device10))));
    ID3D12Resource *image = nullptr;
    EQ_CHECK(SUCCEEDED(device10->CreatePlacedResource2(RI::D3D12::native(gpuMemory), 0, &textureDesc,
                                                       D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, 0, nullptr,
                                                       IID_PPV_ARGS(&image))));
    RI::Texture texture = RI::D3D12::importTexture(device, image, desc);

    // A native upload heap, imported; RI places its staging buffer in it.
    const D3D12_HEAP_DESC heapDesc{.SizeInBytes = 1 << 20,
                                   .Properties = {.Type = D3D12_HEAP_TYPE_UPLOAD},
                                   .Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS};
    ID3D12Heap *uploadHeap = nullptr;
    EQ_CHECK(SUCCEEDED(nd->CreateHeap(&heapDesc, IID_PPV_ARGS(&uploadHeap))));
    const D3D12_RESOURCE_DESC bufferDesc{.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
                                         .Width = 1 << 20,
                                         .Height = 1,
                                         .DepthOrArraySize = 1,
                                         .MipLevels = 1,
                                         .SampleDesc = {1, 0},
                                         .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
    ID3D12Resource *whole = nullptr; // maps the whole heap for its CPU address
    EQ_CHECK(SUCCEEDED(nd->CreatePlacedResource(uploadHeap, 0, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                IID_PPV_ARGS(&whole))));
    void *mapped = nullptr;
    EQ_CHECK(SUCCEEDED(whole->Map(0, nullptr, &mapped)));
    RI::Memory uploadMemory = RI::D3D12::importMemory(device, uploadHeap, RI::MemoryKind::eUpload, 1 << 20, mapped);
    RI::Buffer staging = RI::createBuffer(device, uploadMemory, 0, {.size = 64 * 1024, .usage = RI::BufferUsage::eCopySrc});

    // A native committed readback buffer, imported.
    const D3D12_HEAP_PROPERTIES readbackHeap{.Type = D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC readbackDesc = bufferDesc;
    readbackDesc.Width = 64 * 1024;
    ID3D12Resource *readbackResource = nullptr;
    EQ_CHECK(SUCCEEDED(nd->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&readbackResource))));
    void *readbackMapped = nullptr;
    EQ_CHECK(SUCCEEDED(readbackResource->Map(0, nullptr, &readbackMapped)));
    RI::Buffer back = RI::D3D12::importBuffer(device, readbackResource,
                                              {.size = 64 * 1024, .usage = RI::BufferUsage::eCopyDst}, readbackMapped);

    const RI::CopyLayout layout = RI::copyLayout(device, texture.view);
    auto *bytes = static_cast<uint8 *>(staging.mapped);
    for (uint64 i = 0; i < layout.size; ++i) {
        bytes[i] = static_cast<uint8>(i * 7);
    }
    runOnce(device, [&](RI::CommandList &cmd) {
        // The imported texture goes to RI's resting layout before RI uses it.
        const RI::D3D12::NativeCommands n = RI::D3D12::beginNative(cmd);
        textureBarrier(n.list, image, D3D12_BARRIER_LAYOUT_UNDEFINED, RI::D3D12::native(texture).restingLayout,
                       D3D12_BARRIER_ACCESS_COMMON);
        RI::D3D12::endNative(cmd);
        RI::cmdCopyBufferToTexture(cmd, staging, 0, texture.view);
        RI::cmdBarrier(cmd, RI::Stage::eCopy, RI::Stage::eCopy);
        RI::cmdCopyTextureToBuffer(cmd, texture.view, back, 0);
    });
    const auto *out = static_cast<const uint8 *>(back.mapped);
    uint32 wrong = 0;
    for (uint32 y = 0; y < 16; ++y) {
        for (uint32 x = 0; x < 64; ++x) {
            const uint64 at = y * layout.rowPitch + x;
            wrong += out[at] != static_cast<uint8>(at * 7);
        }
    }
    EQ_CHECK(wrong == 0);

    const std::vector<std::byte> shader = gputest::loadShader("write_storage");
    RI::Pipeline pipeline = RI::createComputePipeline(device, {.shader = shader});
    RI::Pipeline imported = RI::D3D12::importPipeline(device, RI::D3D12::native(pipeline).pipelineState,
                                                      RI::D3D12::native(pipeline).rootSignature, false,
                                                      pipeline.rootDataSize);
    EQ_CHECK(imported.rootDataSize == sizeof(WriteRoot));
    RI::destroyPipeline(device, imported);
    RI::destroyPipeline(device, pipeline);

    RI::destroyBuffer(device, back);       // forgets only
    RI::destroyBuffer(device, staging);
    RI::freeMemory(device, uploadMemory);  // forgets only
    RI::destroyTexture(device, texture);   // frees RI's descriptors, forgets the resource
    readbackResource->Release();
    whole->Release();
    uploadHeap->Release();
    image->Release();
    device10->Release();
    RI::freeMemory(device, gpuMemory);
    RI::destroyDevice(device);
}

// Work graphs (P50, P62): an entry node launched with one CPU record sends 64
// records to a thread node, which adds them into output[0]. Dispatched twice:
// the second launch reuses the backing memory without initializing it.
EQ_TEST(d3d12, work_graph) {
    RI::Device device = RI::createDevice({.gpu = gputest::pickGpu()});
    if (!device.limits.workGraphs) {
        RI::destroyDevice(device);
        return;
    }
    const std::vector<std::byte> library = gputest::loadShader("work_graph");
    struct GraphRoot {
        uint32 outputSlot;
    };
    RI::WorkGraph graph = RI::createWorkGraph(device, {.library = library, .rootDataSize = sizeof(GraphRoot)});
    EQ_CHECK(graph.backing.minSize > 0 || graph.backing.maxSize == 0);
    const uint32 entry = RI::workGraphEntry(graph, "entry");
    gputest::Arena gpu{.memory = RI::allocateMemory(device, RI::MemoryKind::eGPU, 64 << 20)};
    gputest::Arena readback{.memory = RI::allocateMemory(device, RI::MemoryKind::eReadback, 1 << 20)};
    const uint64 backingSize = std::max<uint64>(graph.backing.minSize, 64);
    RI::Buffer backing = gputest::placeBuffer(device, gpu, {.size = backingSize, .usage = RI::BufferUsage::eReadWrite});
    RI::Buffer output = gputest::placeBuffer(device, gpu, {.size = 256,
                                                           .usage = RI::BufferUsage::eReadWrite | RI::BufferUsage::eCopySrc |
                                                                    RI::BufferUsage::eCopyDst});
    RI::Buffer result = gputest::placeBuffer(device, readback, {.size = 256, .usage = RI::BufferUsage::eCopyDst});
    RI::DescriptorHeap heap = RI::createDescriptorHeap(device, {.resourceCount = 4});
    RI::writeDescriptors(device, heap, {{.slot = 0, .descriptor = RI::ReadWriteView{.buffer = output}}});
    const uint32 multiplier = 2;
    RI::bindWorkGraphMemory(device, graph, backing, 0, backingSize); // initialized here, once
    runOnce(device, [&](RI::CommandList &cmd) {
        RI::cmdClearBuffer(cmd, output, 0);
        RI::cmdBarrier(cmd, RI::Stage::eCopy, RI::Stage::eCompute);
        RI::cmdSetHeap(cmd, heap);
        for (int launch = 0; launch < 2; ++launch) {
            RI::cmdSetWorkGraph(cmd, graph);
            RI::cmdSetRootData(cmd, GraphRoot{.outputSlot = 0});
            const RI::GraphInput input{.entry = entry, .records = &multiplier, .recordCount = 1, .recordStride = 4};
            RI::cmdDispatchGraph(cmd, {&input, 1});
            RI::cmdBarrier(cmd, RI::Stage::eCompute, RI::Stage::eCompute);
        }
        RI::cmdBarrier(cmd, RI::Stage::eCompute, RI::Stage::eCopy);
        RI::cmdCopyBuffer(cmd, output, 0, result, 0, 256);
    });
    EQ_CHECK(*static_cast<const uint32 *>(result.mapped) == 2 * 2 * (63 * 64 / 2));

    RI::destroyDescriptorHeap(device, heap);
    RI::destroyBuffer(device, result);
    RI::destroyBuffer(device, output);
    RI::destroyBuffer(device, backing);
    RI::freeMemory(device, readback.memory);
    RI::freeMemory(device, gpu.memory);
    RI::destroyWorkGraph(device, graph);
    RI::destroyDevice(device);
}
