// Pipeline-statistics helper (P41, P71, P105): reads each counter at the offset the
// backend reports.

#include <RI/Helpers/Statistics.hpp>

#include <cstddef>
#include <cstring>

namespace RI::Helpers {

PipelineStatistics readPipelineStatistics(const PipelineStatisticsLayout &layout, const void *result) {
    const auto read = [result](uint32 offset) -> uint64 {
        if (offset == kNoCounter) {
            return 0;
        }
        uint64 value = 0;
        std::memcpy(&value, static_cast<const std::byte *>(result) + offset, sizeof(value));
        return value;
    };
    return {.inputVertices = read(layout.inputVertices),
            .inputPrimitives = read(layout.inputPrimitives),
            .vertexShaderInvocations = read(layout.vertexShaderInvocations),
            .clippingInvocations = read(layout.clippingInvocations),
            .clippingPrimitives = read(layout.clippingPrimitives),
            .pixelShaderInvocations = read(layout.pixelShaderInvocations),
            .computeShaderInvocations = read(layout.computeShaderInvocations),
            .geometryShaderInvocations = read(layout.geometryShaderInvocations),
            .geometryShaderPrimitives = read(layout.geometryShaderPrimitives),
            .hullShaderInvocations = read(layout.hullShaderInvocations),
            .domainShaderInvocations = read(layout.domainShaderInvocations),
            .meshPrimitives = read(layout.meshPrimitives)};
}

} // namespace RI::Helpers
