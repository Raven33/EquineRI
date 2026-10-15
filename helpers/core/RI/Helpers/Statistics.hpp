#pragma once

// Pipeline-statistics helper (P41, P71, P105): one result of a counter query
// (ePipelineStatistics, eMeshPipelineStatistics, eMeshPrimitives), in the order
// the backend wrote it, read into one fixed struct on the CPU. RI itself only
// reports where each counter is (pipelineStatisticsLayout(device, type)).

#include <RI/RI.hpp>

namespace RI::Helpers {

// Counters of one counter query. A counter the query type does not hold, or
// the GPU lacks, reads 0.
struct PipelineStatistics {
    uint64 inputVertices = 0;
    uint64 inputPrimitives = 0;
    uint64 vertexShaderInvocations = 0;
    uint64 clippingInvocations = 0;
    uint64 clippingPrimitives = 0;
    uint64 pixelShaderInvocations = 0;
    uint64 computeShaderInvocations = 0;
    uint64 geometryShaderInvocations = 0;
    uint64 geometryShaderPrimitives = 0;
    uint64 hullShaderInvocations = 0;
    uint64 domainShaderInvocations = 0;
    uint64 meshPrimitives = 0;
};

// `result` points at one query's result as cmdResolveQueries copied it
// (`layout.size` bytes), once its timeline point is reached.
PipelineStatistics readPipelineStatistics(const PipelineStatisticsLayout &layout, const void *result);

} // namespace RI::Helpers
