// Canonical D3D12: buffer -> compute -> readback, the same program as
// snippets/ri/compute.cpp written with D3D12 alone (no RI). Bindless with
// shader model 6.6 ResourceDescriptorHeap[] and enhanced barriers. Shader:
// snippets/hlsl/doc_compute.hlsl compiled with
//   dxc -T cs_6_6 -E main -Fo doc_compute.dxil doc_compute.hlsl
//
// A stand-alone program (Agility SDK exports as in snippets/d3d12/device.cpp).
// Error handling is reduced to "stop on failure" to keep the example short.
// Compiled against EquineD3D12Backend for the pinned Agility SDK headers.

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

using Microsoft::WRL::ComPtr;

extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = D3D12_SDK_VERSION;
__declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";
}

namespace {

void check(HRESULT result, const char *what) {
    if (FAILED(result)) {
        std::fprintf(stderr, "%s failed (0x%08lX)\n", what, static_cast<unsigned long>(result));
        std::exit(1);
    }
}

std::vector<char> loadDxil(const char *path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// --8<-- [start:allocate-function]
// One block of memory: a heap of a given type. Buffers are placed in it later.
ComPtr<ID3D12Heap> allocate(ID3D12Device10 *device, D3D12_HEAP_TYPE type, UINT64 size) {
    const D3D12_HEAP_DESC desc{
        .SizeInBytes = (size + 65535) / 65536 * 65536, // whole 64 KiB blocks
        .Properties = {.Type = type},
        .Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
        .Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS,
    };
    ComPtr<ID3D12Heap> heap;
    check(device->CreateHeap(&desc, IID_PPV_ARGS(&heap)), "CreateHeap");
    return heap;
}
// --8<-- [end:allocate-function]

// --8<-- [start:buffer-desc]
D3D12_RESOURCE_DESC1 bufferDesc(UINT64 size, D3D12_RESOURCE_FLAGS flags) {
    return {.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
            .Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
            .Width = size,
            .Height = 1,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .Format = DXGI_FORMAT_UNKNOWN,
            .SampleDesc = {.Count = 1, .Quality = 0},
            .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
            .Flags = flags}; // ALLOW_UNORDERED_ACCESS for read-write (UAV) use
}
// --8<-- [end:buffer-desc]

// --8<-- [start:create-buffer-function]
// A buffer placed in `heap` at `offset`. Buffers have no layout: UNDEFINED.
ComPtr<ID3D12Resource> createBuffer(ID3D12Device10 *device, ID3D12Heap *heap, UINT64 offset,
                                    const D3D12_RESOURCE_DESC1 &desc) {
    ComPtr<ID3D12Resource> buffer;
    check(device->CreatePlacedResource2(heap, offset, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, 0, nullptr,
                                        IID_PPV_ARGS(&buffer)),
          "CreatePlacedResource2");
    return buffer;
}
// --8<-- [end:create-buffer-function]

// The C++ mirror of the shader's root data.
struct Root {
    uint32_t inputSlot;
    uint32_t outputSlot;
    uint32_t paramsSlot;
    float scale;
    uint32_t count;
};

} // namespace

int main(int argc, char **argv) {
    const char *shaderPath = argc > 1 ? argv[1] : "doc_compute.dxil";

    // Device: see the device-creation snippet (snippets/d3d12/device.cpp).
    ComPtr<IDXGIFactory6> factory;
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
    ComPtr<IDXGIAdapter1> adapter;
    check(factory->EnumAdapters1(0, &adapter), "EnumAdapters1");
    ComPtr<ID3D12Device10> device; // ID3D12Device10: placed resources with layouts (Agility SDK)
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
    const D3D12_COMMAND_QUEUE_DESC queueDesc{.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
    ComPtr<ID3D12CommandQueue> queue;
    check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");

    constexpr uint32_t kCount = 4096;
    constexpr UINT64 kBytes = kCount * sizeof(float);

    // --8<-- [start:requirements]
    // Size and alignment of a placed buffer (64 KiB alignment for buffers).
    const D3D12_RESOURCE_DESC1 dataDesc = bufferDesc(kBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_ALLOCATION_INFO info = device->GetResourceAllocationInfo2(0, 1, &dataDesc, nullptr);
    const UINT64 outputOffset = (info.SizeInBytes + info.Alignment - 1) / info.Alignment * info.Alignment;
    // --8<-- [end:requirements]
    const UINT64 paramsOffset = (kBytes + 65535) / 65536 * 65536;

    // --8<-- [start:allocate-memory]
    // DEFAULT: GPU memory. UPLOAD: CPU writes, GPU reads. READBACK: GPU writes, CPU reads.
    ComPtr<ID3D12Heap> gpuMemory = allocate(device.Get(), D3D12_HEAP_TYPE_DEFAULT, outputOffset + info.SizeInBytes);
    ComPtr<ID3D12Heap> uploadMemory = allocate(device.Get(), D3D12_HEAP_TYPE_UPLOAD, paramsOffset + 256);
    ComPtr<ID3D12Heap> readbackMemory = allocate(device.Get(), D3D12_HEAP_TYPE_READBACK, kBytes);
    // --8<-- [end:allocate-memory]

    // --8<-- [start:create-buffers]
    ComPtr<ID3D12Resource> input = createBuffer(device.Get(), gpuMemory.Get(), 0, dataDesc);
    ComPtr<ID3D12Resource> output = createBuffer(device.Get(), gpuMemory.Get(), outputOffset, dataDesc);
    ComPtr<ID3D12Resource> staging =
        createBuffer(device.Get(), uploadMemory.Get(), 0, bufferDesc(kBytes, D3D12_RESOURCE_FLAG_NONE));
    ComPtr<ID3D12Resource> params =
        createBuffer(device.Get(), uploadMemory.Get(), paramsOffset, bufferDesc(256, D3D12_RESOURCE_FLAG_NONE));
    ComPtr<ID3D12Resource> result =
        createBuffer(device.Get(), readbackMemory.Get(), 0, bufferDesc(kBytes, D3D12_RESOURCE_FLAG_NONE));
    const D3D12_GPU_VIRTUAL_ADDRESS inputAddress = input->GetGPUVirtualAddress(); // every buffer has one
    // --8<-- [end:create-buffers]
    std::printf("input at GPU address 0x%llx\n", static_cast<unsigned long long>(inputAddress));

    // --8<-- [start:write-mapped]
    // D3D12 maps resources, not heaps. UPLOAD and READBACK resources may stay
    // mapped for their whole lifetime. The read range {0, 0}: the CPU reads nothing.
    void *stagingMapped = nullptr;
    void *paramsMapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    check(staging->Map(0, &noRead, &stagingMapped), "Map");
    check(params->Map(0, &noRead, &paramsMapped), "Map");
    auto *values = static_cast<float *>(stagingMapped);
    for (uint32_t i = 0; i < kCount; ++i) {
        values[i] = static_cast<float>(i);
    }
    const float offset = 0.25f;
    std::memcpy(paramsMapped, &offset, sizeof(offset));
    // --8<-- [end:write-mapped]

    // --8<-- [start:create-heap]
    // Shader-visible descriptor heaps: one for views (CBV/SRV/UAV), one for samplers.
    const D3D12_DESCRIPTOR_HEAP_DESC viewHeapDesc{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                                  .NumDescriptors = 16,
                                                  .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    const D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                                                     .NumDescriptors = 16,
                                                     .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    ComPtr<ID3D12DescriptorHeap> viewHeap;
    ComPtr<ID3D12DescriptorHeap> samplerHeap;
    check(device->CreateDescriptorHeap(&viewHeapDesc, IID_PPV_ARGS(&viewHeap)), "CreateDescriptorHeap");
    check(device->CreateDescriptorHeap(&samplerHeapDesc, IID_PPV_ARGS(&samplerHeap)), "CreateDescriptorHeap");
    // --8<-- [end:create-heap]

    // --8<-- [start:write-descriptors]
    // A slot's CPU address: heap start + slot * descriptor size (vendor-specific).
    const UINT descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_CPU_DESCRIPTOR_HANDLE start = viewHeap->GetCPUDescriptorHandleForHeapStart();
    const auto slot = [&](UINT index) { return D3D12_CPU_DESCRIPTOR_HANDLE{start.ptr + index * descriptorSize}; };

    // Slot 0: StructuredBuffer<float>. Structured views count elements of StructureByteStride.
    const D3D12_SHADER_RESOURCE_VIEW_DESC srv{
        .Format = DXGI_FORMAT_UNKNOWN,
        .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Buffer = {.FirstElement = 0, .NumElements = kCount, .StructureByteStride = sizeof(float)}};
    device->CreateShaderResourceView(input.Get(), &srv, slot(0));
    // Slot 1: RWStructuredBuffer<float>.
    const D3D12_UNORDERED_ACCESS_VIEW_DESC uav{
        .Format = DXGI_FORMAT_UNKNOWN,
        .ViewDimension = D3D12_UAV_DIMENSION_BUFFER,
        .Buffer = {.FirstElement = 0, .NumElements = kCount, .StructureByteStride = sizeof(float)}};
    device->CreateUnorderedAccessView(output.Get(), nullptr, &uav, slot(1));
    // Slot 2: ConstantBuffer<Params>, by GPU address; 256-byte aligned and sized.
    const D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{.BufferLocation = params->GetGPUVirtualAddress(), .SizeInBytes = 256};
    device->CreateConstantBufferView(&cbv, slot(2));
    // --8<-- [end:write-descriptors]

    // --8<-- [start:create-pipeline]
    // The root signature: root constants at b0, space999 (where EQ_ROOT_DATA puts
    // the root data), and the flags that let shaders index the heaps directly.
    const D3D12_ROOT_PARAMETER1 rootConstants{
        .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
        .Constants = {.ShaderRegister = 0, .RegisterSpace = 999, .Num32BitValues = sizeof(Root) / 4},
        .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
    };
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootDesc{
        .Version = D3D_ROOT_SIGNATURE_VERSION_1_1,
        .Desc_1_1 = {.NumParameters = 1,
                     .pParameters = &rootConstants,
                     .Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                              D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED},
    };
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    check(D3D12SerializeVersionedRootSignature(&rootDesc, &blob, &errors), "D3D12SerializeVersionedRootSignature");
    ComPtr<ID3D12RootSignature> rootSignature;
    check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSignature)),
          "CreateRootSignature");

    const std::vector<char> dxil = loadDxil(shaderPath);
    const D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{
        .pRootSignature = rootSignature.Get(),
        .CS = {.pShaderBytecode = dxil.data(), .BytecodeLength = dxil.size()},
    };
    ComPtr<ID3D12PipelineState> pipeline;
    check(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pipeline)), "CreateComputePipelineState");
    // --8<-- [end:create-pipeline]

    // --8<-- [start:create-command-list]
    // The allocator owns the command memory; the list records into it.
    ComPtr<ID3D12CommandAllocator> allocator;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
          "CreateCommandAllocator");
    ComPtr<ID3D12GraphicsCommandList7> cmd; // version 7: enhanced barriers
    check(device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE,
                                     IID_PPV_ARGS(&cmd)),
          "CreateCommandList1"); // created closed
    // --8<-- [end:create-command-list]

    // --8<-- [start:barrier-function]
    // A global barrier: no resource named. Buffers have no layouts, so this is all they need.
    const auto barrier = [&](D3D12_BARRIER_SYNC syncBefore, D3D12_BARRIER_ACCESS accessBefore,
                             D3D12_BARRIER_SYNC syncAfter, D3D12_BARRIER_ACCESS accessAfter) {
        D3D12_GLOBAL_BARRIER global{.SyncBefore = syncBefore,
                                    .SyncAfter = syncAfter,
                                    .AccessBefore = accessBefore,
                                    .AccessAfter = accessAfter};
        const D3D12_BARRIER_GROUP group{.Type = D3D12_BARRIER_TYPE_GLOBAL, .NumBarriers = 1, .pGlobalBarriers = &global};
        cmd->Barrier(1, &group);
    };
    // --8<-- [end:barrier-function]

    // --8<-- [start:record]
    // --8<-- [start:cmd-begin]
    check(cmd->Reset(allocator.Get(), nullptr), "ID3D12GraphicsCommandList::Reset"); // opens it for recording
    // --8<-- [end:cmd-begin]
    // --8<-- [start:copy-in]
    cmd->CopyBufferRegion(input.Get(), 0, staging.Get(), 0, kBytes); // destination first
    // --8<-- [end:copy-in]
    // --8<-- [start:barrier-copy-compute]
    barrier(D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_DEST, D3D12_BARRIER_SYNC_COMPUTE_SHADING,
            D3D12_BARRIER_ACCESS_SHADER_RESOURCE | D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
    // --8<-- [end:barrier-copy-compute]
    // --8<-- [start:set-heap]
    ID3D12DescriptorHeap *heaps[] = {viewHeap.Get(), samplerHeap.Get()};
    cmd->SetDescriptorHeaps(2, heaps); // before the root signature that indexes them
    // --8<-- [end:set-heap]
    // --8<-- [start:set-pipeline]
    cmd->SetComputeRootSignature(rootSignature.Get());
    cmd->SetPipelineState(pipeline.Get());
    // --8<-- [end:set-pipeline]
    // --8<-- [start:root-data]
    const Root root{.inputSlot = 0, .outputSlot = 1, .paramsSlot = 2, .scale = 3.0f, .count = kCount};
    cmd->SetComputeRoot32BitConstants(0, sizeof(root) / 4, &root, 0);
    // --8<-- [end:root-data]
    // --8<-- [start:dispatch]
    cmd->Dispatch((kCount + 63) / 64, 1, 1);
    // --8<-- [end:dispatch]
    // --8<-- [start:barrier-compute-copy]
    barrier(D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_SYNC_COPY,
            D3D12_BARRIER_ACCESS_COPY_SOURCE);
    // --8<-- [end:barrier-compute-copy]
    // --8<-- [start:copy-out]
    cmd->CopyBufferRegion(result.Get(), 0, output.Get(), 0, kBytes);
    // --8<-- [end:copy-out]
    // --8<-- [start:cmd-end]
    // No barrier to the CPU: once the fence value is reached, mapped reads see the writes.
    check(cmd->Close(), "ID3D12GraphicsCommandList::Close");
    // --8<-- [end:cmd-end]
    // --8<-- [end:record]

    // --8<-- [start:timeline]
    // A fence: a 64-bit counter the queue raises as work completes.
    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    UINT64 lastSignaled = 0;
    // --8<-- [end:timeline]

    // --8<-- [start:submit]
    ID3D12CommandList *lists[] = {cmd.Get()};
    queue->ExecuteCommandLists(1, lists);
    const UINT64 done = ++lastSignaled;
    check(queue->Signal(fence.Get(), done), "Signal"); // raised when the work before it is done
    // --8<-- [end:submit]

    // --8<-- [start:wait]
    if (fence->GetCompletedValue() < done) {
        // A null event: the call blocks until the fence reaches the value.
        check(fence->SetEventOnCompletion(done, nullptr), "SetEventOnCompletion");
    }
    // --8<-- [end:wait]

    // --8<-- [start:is-reached]
    const bool finished = fence->GetCompletedValue() >= done; // never blocks (UINT64_MAX: device removed)
    // --8<-- [end:is-reached]

    // --8<-- [start:read-back]
    void *resultMapped = nullptr;
    const D3D12_RANGE readAll{0, static_cast<SIZE_T>(kBytes)};
    check(result->Map(0, &readAll, &resultMapped), "Map");
    const auto *out = static_cast<const float *>(resultMapped);
    std::printf("out[1] = %g (expected 3.25), finished: %d\n", out[1], finished ? 1 : 0);
    // --8<-- [end:read-back]

    // --8<-- [start:reset]
    // Only after the GPU is done with it: the allocator's memory is reused.
    check(allocator->Reset(), "ID3D12CommandAllocator::Reset");
    check(cmd->Reset(allocator.Get(), nullptr), "ID3D12GraphicsCommandList::Reset");
    check(cmd->Close(), "ID3D12GraphicsCommandList::Close");
    // --8<-- [end:reset]

    // --8<-- [start:destroy]
    // COM reference counting: Reset() releases (so does leaving the scope).
    // Release nothing the GPU may still use: the fence value is reached here.
    fence.Reset();
    cmd.Reset();
    allocator.Reset();
    pipeline.Reset();
    rootSignature.Reset();
    viewHeap.Reset();
    samplerHeap.Reset();
    for (ComPtr<ID3D12Resource> *buffer : {&input, &output, &staging, &params, &result}) {
        buffer->Reset(); // placed resources before their heaps
    }
    gpuMemory.Reset();
    uploadMemory.Reset();
    readbackMemory.Reset();
    // --8<-- [end:destroy]
    return 0;
}
