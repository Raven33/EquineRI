// What the Metal backend does not build yet (P113).
//
// Owns: notYet, and a definition of every function of RI/Metal/Backend.hpp
// that stages 2 (buffers, compute, readback) and 3a (textures, views,
// samplers, copies, clears, timestamps) do not cover: acceleration
// structures, graphics pipelines, rendering and draws, indirect execution,
// resolves, occlusion queries, swapchains and work graphs. Each reports
// eUnsupported, so programs link and fail with a readable message. Capability
// probes (presentModeSupported, screenStorageSupported) answer false instead,
// so portable code takes its fallback as on the other backends.
//
// Watch out: these do no thread or liveness checks: every call stops at
// reportError anyway. Work graphs are not a stage still to come: they are
// D3D12 only (P50), worded as on Vulkan. When a function is built, move it
// to its file and delete it here, or the link fails with a duplicate.
//
// Vulkan counterpart of the work-graph part: backends/vulkan/WorkGraph.cpp.

#include "DeviceState.hpp"

#include <string>

namespace RI::Metal {

void notYet(std::string_view call) {
    reportError(ErrorCategory::eUnsupported,
                std::string(call) +
                    ": not built yet on the Metal backend (stages 2 and 3a cover compute and textures; see "
                    "docs/FeatureProposal-Metal-2026-11-07.md)");
}

namespace {

[[noreturn]] void d3d12Only(const char *call) {
    reportError(ErrorCategory::eUnsupported,
                std::string(call) + ": work graphs are D3D12 only (P50; DeviceLimits::workGraphs is false on Metal)");
}

} // namespace

// ── Acceleration structures ──────────────────────────────────────────────────

AccelerationStructureSizes blasSizes(Device &, const BlasDesc &) { notYet("blasSizes"); }
AccelerationStructureSizes tlasSizes(Device &, const TlasDesc &) { notYet("tlasSizes"); }
MemoryRequirements accelerationStructureRequirements(Device &, uint64) {
    notYet("accelerationStructureRequirements");
}
AccelerationStructure createAccelerationStructure(Device &, const Memory &, uint64, uint64, bool, std::string_view) {
    notYet("createAccelerationStructure");
}
void destroyAccelerationStructure(Device &, AccelerationStructure &) { notYet("destroyAccelerationStructure"); }
void cmdBuildBlas(CommandList &, const AccelerationStructure &, const BlasDesc &, const Buffer &, uint64, bool) {
    notYet("cmdBuildBlas");
}
void cmdBuildTlas(CommandList &, const AccelerationStructure &, const TlasDesc &, const Buffer &, uint64, bool) {
    notYet("cmdBuildTlas");
}

// ── Graphics pipelines, rendering and draws ──────────────────────────────────

Pipeline createGraphicsPipeline(Device &, const GraphicsPipelineDesc &) { notYet("createGraphicsPipeline"); }
void cmdBeginRendering(CommandList &, const RenderingDesc &) { notYet("cmdBeginRendering"); }
void cmdBeginRendering(CommandList &, RenderingSize) { notYet("cmdBeginRendering"); }
void cmdEndRendering(CommandList &) { notYet("cmdEndRendering"); }
void cmdSetViewport(CommandList &, const Viewport &) { notYet("cmdSetViewport"); }
void cmdSetScissor(CommandList &, const Rect &) { notYet("cmdSetScissor"); }
void cmdSetStencilReference(CommandList &, uint8) { notYet("cmdSetStencilReference"); }
void cmdDraw(CommandList &, uint32, uint32, uint32, uint32) { notYet("cmdDraw"); }
void cmdSetIndexBuffer(CommandList &, const Buffer &, uint64, IndexFormat) { notYet("cmdSetIndexBuffer"); }
void cmdDrawIndexed(CommandList &, uint32, uint32, uint32, int32, uint32) { notYet("cmdDrawIndexed"); }
void cmdDrawMesh(CommandList &, uint32, uint32, uint32) { notYet("cmdDrawMesh"); }

// ── Indirect execution ───────────────────────────────────────────────────────

void cmdExecuteIndirect(CommandList &, IndirectKind, const Buffer &, uint64, uint32, const Buffer *, uint64) {
    notYet("cmdExecuteIndirect");
}
MemoryRequirements indirectScratchRequirements(Device &, const Pipeline &, IndirectKind, uint32) {
    notYet("indirectScratchRequirements");
}
void cmdExecuteIndirectWithRootData(CommandList &, IndirectKind, const Buffer &, uint64, uint32, const Buffer *, uint64,
                                    const Buffer *, uint64) {
    notYet("cmdExecuteIndirectWithRootData");
}

// ── Resolves ─────────────────────────────────────────────────────────────────

void cmdResolve(CommandList &, const TextureView &, const TextureView &, ResolveMode) { notYet("cmdResolve"); }

// ── Occlusion queries (with rendering) ───────────────────────────────────────

void cmdBeginQuery(CommandList &, const QueryPool &, uint32) { notYet("cmdBeginQuery"); }
void cmdEndQuery(CommandList &, const QueryPool &, uint32) { notYet("cmdEndQuery"); }

// ── Swapchains ───────────────────────────────────────────────────────────────

std::vector<SurfaceFormat> surfaceFormats(Device &, const Window &) { notYet("surfaceFormats"); }
bool presentModeSupported(Device &, const Window &, PresentMode) { return false; }
bool screenStorageSupported(Device &, const Window &, Format) { return false; }
Swapchain createSwapchain(Device &, const SwapchainDesc &) { notYet("createSwapchain"); }
void resizeSwapchain(Device &, Swapchain &, uint32, uint32) { notYet("resizeSwapchain"); }
void destroySwapchain(Device &, Swapchain &) { notYet("destroySwapchain"); }
Texture acquire(Device &, Swapchain &) { notYet("acquire"); }
void present(Queue &, Swapchain &) { notYet("present"); }

// ── Work graphs (P50): D3D12 only ────────────────────────────────────────────

WorkGraph createWorkGraph(Device &, const WorkGraphDesc &) { d3d12Only("createWorkGraph"); }
void destroyWorkGraph(Device &, WorkGraph &) { d3d12Only("destroyWorkGraph"); }
uint32 workGraphEntry(const WorkGraph &, std::string_view, uint32) { d3d12Only("workGraphEntry"); }
void bindWorkGraphMemory(Device &, WorkGraph &, const Buffer &, uint64, uint64) { d3d12Only("bindWorkGraphMemory"); }
void cmdSetWorkGraph(CommandList &, const WorkGraph &) { d3d12Only("cmdSetWorkGraph"); }
void cmdDispatchGraph(CommandList &, std::span<const GraphInput>) { d3d12Only("cmdDispatchGraph"); }
void cmdDispatchGraphIndirect(CommandList &, uint64, bool) { d3d12Only("cmdDispatchGraphIndirect"); }

} // namespace RI::Metal
