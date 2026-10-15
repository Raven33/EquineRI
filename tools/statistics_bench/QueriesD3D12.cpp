// Statistics-cost measurement, D3D12 side (P105): variant B as a
// PIPELINE_STATISTICS query heap (as RI records it) and C as PIPELINE_STATISTICS1, recorded in native sections (P70). D3D12 has
// no separate mesh-primitives query (C includes it) and no counter selection,
// so D and E do not exist here.
//
// Vulkan counterpart: QueriesVulkan.cpp.

#include "Queries.hpp"

#include <RI/D3D12/Native.hpp>

namespace statbench {

struct NativeQueries {
    uint32 count = 0;
    D3D12_QUERY_TYPE type = D3D12_QUERY_TYPE_PIPELINE_STATISTICS;
    ID3D12QueryHeap *heap = nullptr;
};

NativeQueries *createNativeQueries(RI::Device &device, Variant variant, uint32 count) {
    ID3D12Device *d3d = RI::D3D12::native(device).device;
    D3D12_QUERY_HEAP_TYPE heapType = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
    D3D12_QUERY_TYPE type = D3D12_QUERY_TYPE_PIPELINE_STATISTICS;
    if (variant == Variant::eStatsMesh) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS9 options9{};
        if (FAILED(d3d->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &options9, sizeof(options9))) ||
            !options9.MeshShaderPipelineStatsSupported) {
            return nullptr;
        }
        heapType = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1;
        type = D3D12_QUERY_TYPE_PIPELINE_STATISTICS1;
    } else if (variant != Variant::eStats) {
        return nullptr;
    }
    auto *q = new NativeQueries{.count = count, .type = type};
    const D3D12_QUERY_HEAP_DESC desc{.Type = heapType, .Count = count};
    if (FAILED(d3d->CreateQueryHeap(&desc, IID_PPV_ARGS(&q->heap)))) {
        delete q;
        return nullptr;
    }
    return q;
}

void destroyNativeQueries(RI::Device & /*device*/, NativeQueries *queries) {
    if (queries == nullptr) {
        return;
    }
    queries->heap->Release();
    delete queries;
}

void beginNativeQuery(NativeQueries &queries, RI::CommandList &cmd, uint32 index, bool /*mesh*/) {
    RI::D3D12::beginNative(cmd).list->BeginQuery(queries.heap, queries.type, index);
    RI::D3D12::endNative(cmd);
}

void endNativeQuery(NativeQueries &queries, RI::CommandList &cmd, uint32 index, bool /*mesh*/) {
    RI::D3D12::beginNative(cmd).list->EndQuery(queries.heap, queries.type, index);
    RI::D3D12::endNative(cmd);
}

void resolveNativeQueries(NativeQueries &queries, RI::CommandList &cmd, const RI::Buffer &buffer) {
    RI::D3D12::beginNative(cmd).list->ResolveQueryData(queries.heap, queries.type, 0,
                                                       queries.count, RI::D3D12::native(buffer), 0);
    RI::D3D12::endNative(cmd);
}

void resetNativeQueries(RI::Device & /*device*/, NativeQueries & /*queries*/) {} // D3D12 queries need no reset

} // namespace statbench
