// Canonical D3D12: textures, views, rendering, a graphics pipeline, clears,
// discard and timestamps, the same program as snippets/ri/textures.cpp
// written with D3D12 alone (no RI). Enhanced barriers: every texture layout
// change is a D3D12_TEXTURE_BARRIER written by hand. Shaders:
// snippets/hlsl/doc_triangle_vs.hlsl and doc_triangle_ps.hlsl, compiled with
//   dxc -T vs_6_6 -E main -Fo doc_triangle_vs.dxil doc_triangle_vs.hlsl
// (and ps_6_6 for the pixel shader).
//
// A stand-alone program (Agility SDK exports as in snippets/d3d12/device.cpp).
// Error handling is reduced to "stop on failure" to keep the example short.
// Compiled against EquineD3D12Backend for the pinned Agility SDK headers.

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

UINT64 alignUp(UINT64 value, UINT64 alignment) { return (value + alignment - 1) / alignment * alignment; }

// --8<-- [start:texture-desc-function]
// A 2D texture description. Flags say what else than sampling it allows.
D3D12_RESOURCE_DESC1 textureDesc(UINT width, UINT height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags) {
    return {.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
            .Alignment = 0, // the default: 64 KiB (4 MiB for MSAA)
            .Width = width,
            .Height = height,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .Format = format,
            .SampleDesc = {.Count = 1, .Quality = 0},
            .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN, // the driver's own (optimal) layout
            .Flags = flags};
}
// --8<-- [end:texture-desc-function]

// --8<-- [start:texture-barrier-function]
// One layout change of a whole texture (every mip, layer and plane), with the
// sync and access on each side. `flags`: DISCARD drops the old contents.
void textureBarrier(ID3D12GraphicsCommandList7 *cmd, ID3D12Resource *texture, D3D12_BARRIER_LAYOUT before,
                    D3D12_BARRIER_LAYOUT after, D3D12_BARRIER_SYNC syncBefore, D3D12_BARRIER_ACCESS accessBefore,
                    D3D12_BARRIER_SYNC syncAfter, D3D12_BARRIER_ACCESS accessAfter,
                    D3D12_TEXTURE_BARRIER_FLAGS flags = D3D12_TEXTURE_BARRIER_FLAG_NONE) {
    const D3D12_TEXTURE_BARRIER barrier{
        .SyncBefore = syncBefore,
        .SyncAfter = syncAfter,
        .AccessBefore = accessBefore,
        .AccessAfter = accessAfter,
        .LayoutBefore = before,
        .LayoutAfter = after,
        .pResource = texture,
        .Subresources = {.IndexOrFirstMipLevel = 0xffffffff}, // all subresources
        .Flags = flags,
    };
    const D3D12_BARRIER_GROUP group{.Type = D3D12_BARRIER_TYPE_TEXTURE, .NumBarriers = 1, .pTextureBarriers = &barrier};
    cmd->Barrier(1, &group);
}
// --8<-- [end:texture-barrier-function]

// The C++ mirror of the shaders' root data.
struct DrawRoot {
    float color[4];
    float depth;
};

} // namespace

int main(int argc, char **argv) {
    const char *vertexPath = argc > 2 ? argv[1] : "doc_triangle_vs.dxil";
    const char *pixelPath = argc > 2 ? argv[2] : "doc_triangle_ps.dxil";

    // Device: see the device-creation snippet (snippets/d3d12/device.cpp).
    ComPtr<IDXGIFactory6> factory;
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
    ComPtr<IDXGIAdapter1> adapter;
    check(factory->EnumAdapters1(0, &adapter), "EnumAdapters1");
    ComPtr<ID3D12Device10> device; // ID3D12Device10: placed resources with layouts and castable formats
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
    const D3D12_COMMAND_QUEUE_DESC queueDesc{.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
    ComPtr<ID3D12CommandQueue> queue;
    check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");

    constexpr UINT kWidth = 1280;
    constexpr UINT kHeight = 720;

    // --8<-- [start:check-format]
    // What a format supports, as two sets of bits.
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{.Format = DXGI_FORMAT_D24_UNORM_S8_UINT};
    check(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)), "CheckFeatureSupport");
    const bool d24 = (support.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL) != 0; // true on every D3D12 GPU
    // Sample counts: D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS (NumQualityLevels > 0).
    // --8<-- [end:check-format]
    std::printf("D24S8: %s\n", d24 ? "yes" : "no");

    // --8<-- [start:requirements]
    // A sampled depth texture is created TYPELESS: its depth view and its
    // shader view use different formats of the same bits.
    const D3D12_RESOURCE_DESC1 colorDesc =
        textureDesc(kWidth, kHeight, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    const D3D12_RESOURCE_DESC1 depthDesc =
        textureDesc(kWidth, kHeight, DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    D3D12_RESOURCE_ALLOCATION_INFO1 parts[2] = {};
    const D3D12_RESOURCE_DESC1 descs[2] = {colorDesc, depthDesc};
    // Both at once: the total, and each one's offset in a shared block.
    const D3D12_RESOURCE_ALLOCATION_INFO total = device->GetResourceAllocationInfo2(0, 2, descs, parts);
    // --8<-- [end:requirements]

    // --8<-- [start:create-textures]
    // GPU memory. ALLOW_ALL_BUFFERS_AND_TEXTURES needs resource heap tier 2.
    const D3D12_HEAP_DESC heapDesc{.SizeInBytes = total.SizeInBytes,
                                   .Properties = {.Type = D3D12_HEAP_TYPE_DEFAULT},
                                   .Alignment = total.Alignment,
                                   .Flags = D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES};
    ComPtr<ID3D12Heap> memory;
    check(device->CreateHeap(&heapDesc, IID_PPV_ARGS(&memory)), "CreateHeap");
    // Castable formats: the other formats views may use (here sRGB). An exact
    // list lets the driver keep compression.
    const DXGI_FORMAT castable[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
    ComPtr<ID3D12Resource> color;
    check(device->CreatePlacedResource2(memory.Get(), parts[0].Offset, &colorDesc, D3D12_BARRIER_LAYOUT_UNDEFINED,
                                        nullptr, 1, castable, IID_PPV_ARGS(&color)),
          "CreatePlacedResource2");
    ComPtr<ID3D12Resource> depth;
    check(device->CreatePlacedResource2(memory.Get(), parts[1].Offset, &depthDesc, D3D12_BARRIER_LAYOUT_UNDEFINED,
                                        nullptr, 0, nullptr, IID_PPV_ARGS(&depth)),
          "CreatePlacedResource2");
    // --8<-- [end:create-textures]

    // --8<-- [start:create-views]
    // Attachments are named by CPU descriptors in non-shader-visible heaps:
    // one heap of render-target views (RTV), one of depth-stencil views (DSV).
    const D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV, .NumDescriptors = 8};
    const D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV, .NumDescriptors = 8};
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    check(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)), "CreateDescriptorHeap");
    check(device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&dsvHeap)), "CreateDescriptorHeap");
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE dsvReadOnly{
        dsv.ptr + device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV)};

    const D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
                                                .ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D,
                                                .Texture2D = {.MipSlice = 0}};
    device->CreateRenderTargetView(color.Get(), &rtvDesc, rtv);
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{.Format = DXGI_FORMAT_D32_FLOAT,
                                          .ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D,
                                          .Flags = D3D12_DSV_FLAG_NONE,
                                          .Texture2D = {.MipSlice = 0}};
    device->CreateDepthStencilView(depth.Get(), &dsvDesc, dsv);
    // A second, read-only DSV: depth tested, not written, while shaders sample it.
    dsvDesc.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
    device->CreateDepthStencilView(depth.Get(), &dsvDesc, dsvReadOnly);
    // --8<-- [end:create-views]

    // --8<-- [start:write-descriptors]
    // Shader views go in the shader-visible heap (see snippets/d3d12/compute.cpp):
    // the color texture read as sRGB, the depth texture read as R32_FLOAT.
    const D3D12_DESCRIPTOR_HEAP_DESC viewHeapDesc{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                                  .NumDescriptors = 16,
                                                  .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    ComPtr<ID3D12DescriptorHeap> viewHeap;
    check(device->CreateDescriptorHeap(&viewHeapDesc, IID_PPV_ARGS(&viewHeap)), "CreateDescriptorHeap");
    const UINT viewSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_CPU_DESCRIPTOR_HANDLE slot0 = viewHeap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_SHADER_RESOURCE_VIEW_DESC colorSrv{.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, // castable
                                                   .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
                                                   .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                                                   .Texture2D = {.MostDetailedMip = 0, .MipLevels = 1}};
    device->CreateShaderResourceView(color.Get(), &colorSrv, slot0);
    const D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv{.Format = DXGI_FORMAT_R32_FLOAT,
                                                   .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
                                                   .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                                                   .Texture2D = {.MostDetailedMip = 0, .MipLevels = 1}};
    device->CreateShaderResourceView(depth.Get(), &depthSrv, D3D12_CPU_DESCRIPTOR_HANDLE{slot0.ptr + viewSize});
    // --8<-- [end:write-descriptors]

    // --8<-- [start:create-pipeline]
    // The root signature: root constants at b0, space999 (EQ_ROOT_DATA), heap indexing.
    const D3D12_ROOT_PARAMETER1 rootConstants{
        .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
        .Constants = {.ShaderRegister = 0, .RegisterSpace = 999, .Num32BitValues = sizeof(DrawRoot) / 4},
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

    const std::vector<char> vertex = loadDxil(vertexPath);
    const std::vector<char> pixel = loadDxil(pixelPath);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = rootSignature.Get();
    pso.VS = {vertex.data(), vertex.size()};
    pso.PS = {pixel.data(), pixel.size()};
    pso.BlendState.RenderTarget[0] = {.BlendEnable = FALSE, // no blending: write the color as is
                                      .LogicOpEnable = FALSE,
                                      .SrcBlend = D3D12_BLEND_ONE,
                                      .DestBlend = D3D12_BLEND_ZERO,
                                      .BlendOp = D3D12_BLEND_OP_ADD,
                                      .SrcBlendAlpha = D3D12_BLEND_ONE,
                                      .DestBlendAlpha = D3D12_BLEND_ZERO,
                                      .BlendOpAlpha = D3D12_BLEND_OP_ADD,
                                      .LogicOp = D3D12_LOGIC_OP_NOOP,
                                      .RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL};
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState = {.FillMode = D3D12_FILL_MODE_SOLID,
                           .CullMode = D3D12_CULL_MODE_BACK,
                           .FrontCounterClockwise = FALSE,
                           .DepthClipEnable = TRUE};
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    // No InputLayout: vertices are pulled in the shader (SV_VertexID).
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; // the exact topology is set when drawing
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc = {.Count = 1, .Quality = 0};
    ComPtr<ID3D12PipelineState> pipeline;
    check(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline)), "CreateGraphicsPipelineState");
    // --8<-- [end:create-pipeline]

    // --8<-- [start:create-query-pool]
    const D3D12_QUERY_HEAP_DESC queryDesc{.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = 4};
    ComPtr<ID3D12QueryHeap> queries;
    check(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&queries)), "CreateQueryHeap");
    // Ticks per second, per queue.
    UINT64 ticksPerSecond = 0;
    check(queue->GetTimestampFrequency(&ticksPerSecond), "GetTimestampFrequency");
    // --8<-- [end:create-query-pool]

    // A readback buffer for the results (see snippets/d3d12/compute.cpp for buffers).
    const D3D12_HEAP_PROPERTIES readbackHeap{.Type = D3D12_HEAP_TYPE_READBACK};
    const D3D12_RESOURCE_DESC1 ticksDesc{.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
                                         .Width = 4 * sizeof(UINT64),
                                         .Height = 1,
                                         .DepthOrArraySize = 1,
                                         .MipLevels = 1,
                                         .SampleDesc = {.Count = 1, .Quality = 0},
                                         .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
    ComPtr<ID3D12Resource> ticks;
    check(device->CreateCommittedResource3(&readbackHeap, D3D12_HEAP_FLAG_NONE, &ticksDesc,
                                           D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr,
                                           IID_PPV_ARGS(&ticks)),
          "CreateCommittedResource3");

    ComPtr<ID3D12CommandAllocator> allocator;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
          "CreateCommandAllocator");
    ComPtr<ID3D12GraphicsCommandList7> cmd;
    check(device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE,
                                     IID_PPV_ARGS(&cmd)),
          "CreateCommandList1");
    check(cmd->Reset(allocator.Get(), nullptr), "ID3D12GraphicsCommandList::Reset");

    // --8<-- [start:initial-layout]
    // A new placed texture is UNDEFINED, and render targets and depth textures
    // must be initialized before use: a DISCARD barrier (or a clear or copy).
    textureBarrier(cmd.Get(), color.Get(), D3D12_BARRIER_LAYOUT_UNDEFINED, D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                   D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_SYNC_RENDER_TARGET,
                   D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_TEXTURE_BARRIER_FLAG_DISCARD);
    textureBarrier(cmd.Get(), depth.Get(), D3D12_BARRIER_LAYOUT_UNDEFINED, D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,
                   D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_SYNC_DEPTH_STENCIL,
                   D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE, D3D12_TEXTURE_BARRIER_FLAG_DISCARD);
    // --8<-- [end:initial-layout]

    // --8<-- [start:write-timestamp-begin]
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0); // timestamps have no BeginQuery
    // --8<-- [end:write-timestamp-begin]

    // --8<-- [start:rendering]
    // --8<-- [start:begin-rendering]
    // The textures are in RENDER_TARGET and DEPTH_STENCIL_WRITE already (above);
    // otherwise, a texture barrier into those layouts comes first.
    cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    const float clearColor[4] = {0.1f, 0.1f, 0.1f, 1.0f};
    cmd->ClearRenderTargetView(rtv, clearColor, 0, nullptr); // clears are commands, not load ops
    cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
    cmd->RSSetViewports(1, &viewport);
    cmd->RSSetScissorRects(1, &scissor);
    // --8<-- [end:begin-rendering]
    // --8<-- [start:draw]
    ID3D12DescriptorHeap *heaps[] = {viewHeap.Get()};
    cmd->SetDescriptorHeaps(1, heaps); // before the root signature that indexes it
    cmd->SetGraphicsRootSignature(rootSignature.Get());
    cmd->SetPipelineState(pipeline.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const DrawRoot root{.color = {1.0f, 0.5f, 0.0f, 1.0f}, .depth = 0.5f};
    cmd->SetGraphicsRoot32BitConstants(0, sizeof(root) / 4, &root, 0);
    cmd->DrawInstanced(3, 1, 0, 0);
    // --8<-- [end:draw]
    // --8<-- [start:end-rendering]
    // There is no "end": the next barrier moves the textures to where shaders
    // read them, and makes the attachment writes visible.
    textureBarrier(cmd.Get(), color.Get(), D3D12_BARRIER_LAYOUT_RENDER_TARGET, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
                   D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET,
                   D3D12_BARRIER_SYNC_PIXEL_SHADING | D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                   D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
    textureBarrier(cmd.Get(), depth.Get(), D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,
                   D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ, D3D12_BARRIER_SYNC_DEPTH_STENCIL,
                   D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE,
                   D3D12_BARRIER_SYNC_PIXEL_SHADING | D3D12_BARRIER_SYNC_DEPTH_STENCIL,
                   D3D12_BARRIER_ACCESS_SHADER_RESOURCE | D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ);
    // --8<-- [end:end-rendering]
    // --8<-- [end:rendering]

    // --8<-- [start:read-only-depth]
    // DEPTH_STENCIL_READ allows depth tests and shader reads at once; bind the
    // read-only DSV. Draws must not write depth.
    cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsvReadOnly);
    // ... draws with DepthWriteMask = ZERO, sampling the depth SRV ...
    // --8<-- [end:read-only-depth]

    // --8<-- [start:clears]
    // A render target is cleared in the RENDER_TARGET layout, even outside a pass.
    textureBarrier(cmd.Get(), color.Get(), D3D12_BARRIER_LAYOUT_SHADER_RESOURCE, D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                   D3D12_BARRIER_SYNC_ALL, D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_SYNC_RENDER_TARGET,
                   D3D12_BARRIER_ACCESS_RENDER_TARGET);
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    cmd->ClearRenderTargetView(rtv, black, 0, nullptr);
    // Storage textures and buffers: ClearUnorderedAccessViewFloat/Uint, which
    // need the view both in a shader-visible heap (GPU handle) and in a
    // non-shader-visible one (CPU handle).
    // --8<-- [end:clears]

    // --8<-- [start:discard]
    // A texture that takes over memory another resource used: a barrier from
    // UNDEFINED with the DISCARD flag initializes its compression metadata.
    // Required before its first use, even if the next pass clears it.
    textureBarrier(cmd.Get(), color.Get(), D3D12_BARRIER_LAYOUT_UNDEFINED, D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                   D3D12_BARRIER_SYNC_ALL, D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_SYNC_RENDER_TARGET,
                   D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_TEXTURE_BARRIER_FLAG_DISCARD);
    // --8<-- [end:discard]

    // --8<-- [start:write-timestamp-end]
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    // 64-bit results to the buffer. No reset step: D3D12 query heaps need none.
    cmd->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, ticks.Get(), 0);
    // --8<-- [end:write-timestamp-end]
    check(cmd->Close(), "ID3D12GraphicsCommandList::Close");

    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    ID3D12CommandList *lists[] = {cmd.Get()};
    queue->ExecuteCommandLists(1, lists);
    check(queue->Signal(fence.Get(), 1), "Signal");
    check(fence->SetEventOnCompletion(1, nullptr), "SetEventOnCompletion"); // blocks until done

    // --8<-- [start:read-timestamps]
    void *mapped = nullptr;
    const D3D12_RANGE readRange{0, 2 * sizeof(UINT64)};
    check(ticks->Map(0, &readRange, &mapped), "Map");
    const auto *values = static_cast<const UINT64 *>(mapped);
    std::printf("GPU time: %.3f ms\n",
                static_cast<double>(values[1] - values[0]) * 1000.0 / static_cast<double>(ticksPerSecond));
    // --8<-- [end:read-timestamps]

    // --8<-- [start:destroy]
    // Placed textures before their heap; nothing the GPU still uses (the fence is reached).
    fence.Reset();
    cmd.Reset();
    allocator.Reset();
    ticks.Reset();
    queries.Reset();
    pipeline.Reset();
    rootSignature.Reset();
    viewHeap.Reset();
    rtvHeap.Reset();
    dsvHeap.Reset();
    color.Reset();
    depth.Reset();
    memory.Reset();
    // --8<-- [end:destroy]
    return 0;
}
