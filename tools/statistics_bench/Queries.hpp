#pragma once

// The statistics-cost measurement (P105, docs/FeatureProposal-Sandbox-2026-10-15.md,
// "Cost"): the query variants, recorded natively. RI's own statistics pool
// cannot be used: on Vulkan it holds the vertex-pipeline counters, which must not
// be active during mesh draws (VUID-vkCmdDrawMeshTasksEXT-pipelineStatistics-07076).

#include <RI/RI.hpp>

namespace statbench {

enum class Variant : uint8 {
    eNone,                // A: no query
    eStats,               // B: the counters allowed with mesh draws (Vulkan: pixel and compute; D3D12: PIPELINE_STATISTICS)
    eStatsMesh,           // C: B + task and mesh invocations (D3D12: PIPELINE_STATISTICS1)
    eStatsMeshPrimitives, // D: C + Vulkan's mesh-primitives query on mesh passes (Vulkan only)
    eComputeOnly,         // E: one counter, compute invocations, as the sandbox (Vulkan only)
};

const char *variantName(Variant variant);

// Native query state of one variant (B to E).
struct NativeQueries;

// nullptr when the device cannot run the variant. `count`: queries per frame.
NativeQueries *createNativeQueries(RI::Device &device, Variant variant, uint32 count);
void destroyNativeQueries(RI::Device &device, NativeQueries *queries);
// Around one pass, outside rendering sections. `mesh`: the pass draws with mesh shaders.
void beginNativeQuery(NativeQueries &queries, RI::CommandList &cmd, uint32 index, bool mesh);
void endNativeQuery(NativeQueries &queries, RI::CommandList &cmd, uint32 index, bool mesh);
// Copies every result into `buffer` (eCopyDst, readback), waiting for each
// query on the GPU as RI's cmdResolveQueries does.
void resolveNativeQueries(NativeQueries &queries, RI::CommandList &cmd, const RI::Buffer &buffer);
// After the frame's timeline point: makes the queries usable again.
void resetNativeQueries(RI::Device &device, NativeQueries &queries);

} // namespace statbench
