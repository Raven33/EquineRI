// Statistics-cost measurement, Vulkan side (P105): variants B to H as native
// query pools, recorded in native sections (P70).
//
// Watch out: only the pixel, compute, task and mesh counters may be active
// during mesh draws (VUID-vkCmdDrawMeshTasksEXT-pipelineStatistics-07076), so B
// is RI's counters minus the vertex-pipeline ones, and C differs from B by the
// task and mesh bits only.
//
// D3D12 counterpart: QueriesD3D12.cpp.

#include "Queries.hpp"

#include <RI/Vk/Native.hpp>

namespace statbench {

struct NativeQueries {
    Variant variant = Variant::eNone;
    uint32 count = 0;
    uint32 statsSize = 0; // bytes per statistics result
    VkDevice device = VK_NULL_HANDLE;
    const VolkDeviceTable *fn = nullptr;
    VkQueryPool stats = VK_NULL_HANDLE;
    VkQueryPool primitives = VK_NULL_HANDLE; // D only
};

namespace {

VkPhysicalDeviceMeshShaderFeaturesEXT meshFeatures(const RI::Vk::NativeDevice &native) {
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &mesh};
    vkGetPhysicalDeviceFeatures2(native.physicalDevice, &features);
    return mesh;
}

// RI's counters that may be active during mesh draws.
constexpr VkQueryPipelineStatisticFlags kMeshCompatible =
    VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT | VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;

uint32 popcount(uint32 v) {
    uint32 n = 0;
    for (; v != 0; v &= v - 1) {
        ++n;
    }
    return n;
}

} // namespace

NativeQueries *createNativeQueries(RI::Device &device, Variant variant, uint32 count) {
    const RI::Vk::NativeDevice native = RI::Vk::native(device);
    const VkPhysicalDeviceMeshShaderFeaturesEXT mesh = meshFeatures(native);
    const bool meshQueries = device.limits.meshShaders && mesh.meshShaderQueries == VK_TRUE;
    VkQueryPipelineStatisticFlags bits = 0;
    switch (variant) {
    case Variant::eStats: bits = kMeshCompatible; break;
    case Variant::eStatsMesh:
    case Variant::eStatsMeshPrimitives:
        if (!meshQueries) {
            return nullptr;
        }
        bits = kMeshCompatible | VK_QUERY_PIPELINE_STATISTIC_TASK_SHADER_INVOCATIONS_BIT_EXT |
               VK_QUERY_PIPELINE_STATISTIC_MESH_SHADER_INVOCATIONS_BIT_EXT;
        break;
    case Variant::eComputeOnly: bits = VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT; break;
    case Variant::eStatsPrimitives:
        if (!meshQueries) {
            return nullptr;
        }
        bits = kMeshCompatible;
        break;
    case Variant::eStatsTask:
    case Variant::eStatsMeshInvocations:
        if (!meshQueries) {
            return nullptr;
        }
        bits = kMeshCompatible | (variant == Variant::eStatsTask ? VK_QUERY_PIPELINE_STATISTIC_TASK_SHADER_INVOCATIONS_BIT_EXT
                                                                 : VK_QUERY_PIPELINE_STATISTIC_MESH_SHADER_INVOCATIONS_BIT_EXT);
        break;
    default: return nullptr;
    }
    auto *q = new NativeQueries{.variant = variant,
                                .count = count,
                                .statsSize = 8 * popcount(bits),
                                .device = native.device,
                                .fn = native.functions};
    const VkQueryPoolCreateInfo statsInfo{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                          .queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS,
                                          .queryCount = count,
                                          .pipelineStatistics = bits};
    q->fn->vkCreateQueryPool(q->device, &statsInfo, nullptr, &q->stats);
    if (variant == Variant::eStatsMeshPrimitives || variant == Variant::eStatsPrimitives) {
        const VkQueryPoolCreateInfo primitivesInfo{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                                   .queryType = VK_QUERY_TYPE_MESH_PRIMITIVES_GENERATED_EXT,
                                                   .queryCount = count};
        q->fn->vkCreateQueryPool(q->device, &primitivesInfo, nullptr, &q->primitives);
    }
    resetNativeQueries(device, *q);
    return q;
}

void destroyNativeQueries(RI::Device & /*device*/, NativeQueries *queries) {
    if (queries == nullptr) {
        return;
    }
    queries->fn->vkDestroyQueryPool(queries->device, queries->stats, nullptr);
    if (queries->primitives != VK_NULL_HANDLE) {
        queries->fn->vkDestroyQueryPool(queries->device, queries->primitives, nullptr);
    }
    delete queries;
}

void beginNativeQuery(NativeQueries &queries, RI::CommandList &cmd, uint32 index, bool mesh) {
    const RI::Vk::NativeCommands nc = RI::Vk::beginNative(cmd);
    nc.functions->vkCmdBeginQuery(nc.commandBuffer, queries.stats, index, 0);
    if (mesh && queries.primitives != VK_NULL_HANDLE) {
        nc.functions->vkCmdBeginQuery(nc.commandBuffer, queries.primitives, index, 0);
    }
    RI::Vk::endNative(cmd);
}

void endNativeQuery(NativeQueries &queries, RI::CommandList &cmd, uint32 index, bool mesh) {
    const RI::Vk::NativeCommands nc = RI::Vk::beginNative(cmd);
    if (mesh && queries.primitives != VK_NULL_HANDLE) {
        nc.functions->vkCmdEndQuery(nc.commandBuffer, queries.primitives, index);
    }
    nc.functions->vkCmdEndQuery(nc.commandBuffer, queries.stats, index);
    RI::Vk::endNative(cmd);
}

void resolveNativeQueries(NativeQueries &queries, RI::CommandList &cmd, const RI::Buffer &buffer) {
    const RI::Vk::NativeCommands nc = RI::Vk::beginNative(cmd);
    const VkQueryResultFlags flags = VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT;
    // Statistics first, then the primitive counts, as one RI result would hold them.
    nc.functions->vkCmdCopyQueryPoolResults(nc.commandBuffer, queries.stats, 0, queries.count, RI::Vk::native(buffer), 0,
                                            queries.statsSize, flags);
    if (queries.primitives != VK_NULL_HANDLE) {
        // Only mesh passes began a primitives query, and main.cpp records the
        // compute pass last, so the mesh passes are indices 0 .. count - 2.
        nc.functions->vkCmdCopyQueryPoolResults(nc.commandBuffer, queries.primitives, 0, queries.count - 1,
                                                RI::Vk::native(buffer), uint64{queries.statsSize} * queries.count, 8,
                                                flags);
    }
    RI::Vk::endNative(cmd);
}

void resetNativeQueries(RI::Device & /*device*/, NativeQueries &queries) {
    queries.fn->vkResetQueryPool(queries.device, queries.stats, 0, queries.count);
    if (queries.primitives != VK_NULL_HANDLE) {
        queries.fn->vkResetQueryPool(queries.device, queries.primitives, 0, queries.count);
    }
}

} // namespace statbench
