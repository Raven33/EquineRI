// What the Metal backend does not build yet (P113).
//
// Owns: notYet, and a definition of every function of RI/Metal/Backend.hpp
// that stages 2 (buffers, compute, readback), 3a (textures, views, samplers,
// copies, clears, timestamps), 3b-1 (graphics pipelines, rendering sections,
// draws), 3b-2 (resolves, occlusion queries, indirect execution), ray
// tracing and swapchains do not cover: work graphs. Each reports
// eUnsupported, so programs link and fail with a readable message.
//
// Watch out: these do no thread or liveness checks: every call stops at
// reportError anyway. Work graphs are not a stage still to come: they are
// D3D12 only (P50), worded as on Vulkan. No call uses notYet today; it stays
// for the stages still to come (P113). When a function
// is built, move it to its file and delete it here, or the link fails with a
// duplicate.
//
// Vulkan counterpart of the work-graph part: backends/vulkan/WorkGraph.cpp.

#include "DeviceState.hpp"

#include <string>

namespace RI::Metal {

void notYet(std::string_view call) {
    reportError(ErrorCategory::eUnsupported,
                std::string(call) +
                    ": not built yet on the Metal backend (built: compute, textures, rendering, indirect execution, "
                    "inline ray tracing, swapchains; see docs/FeatureProposal-Metal-2026-11-07.md)");
}

namespace {

[[noreturn]] void d3d12Only(const char *call) {
    reportError(ErrorCategory::eUnsupported,
                std::string(call) + ": work graphs are D3D12 only (P50; DeviceLimits::workGraphs is false on Metal)");
}

} // namespace

// ── Work graphs (P50): D3D12 only ────────────────────────────────────────────

WorkGraph createWorkGraph(Device &, const WorkGraphDesc &) { d3d12Only("createWorkGraph"); }
void destroyWorkGraph(Device &, WorkGraph &) { d3d12Only("destroyWorkGraph"); }
uint32 workGraphEntry(const WorkGraph &, std::string_view, uint32) { d3d12Only("workGraphEntry"); }
void bindWorkGraphMemory(Device &, WorkGraph &, const Buffer &, uint64, uint64) { d3d12Only("bindWorkGraphMemory"); }
void cmdSetWorkGraph(CommandList &, const WorkGraph &) { d3d12Only("cmdSetWorkGraph"); }
void cmdDispatchGraph(CommandList &, std::span<const GraphInput>) { d3d12Only("cmdDispatchGraph"); }
void cmdDispatchGraphIndirect(CommandList &, uint64, bool) { d3d12Only("cmdDispatchGraphIndirect"); }

} // namespace RI::Metal
