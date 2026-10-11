// Work graphs on Vulkan (P50): every call reports unsupported
// (VK_AMDX_shader_enqueue is provisional and AMD-only). Kept so both backends
// declare the same functions.
//
// D3D12 counterpart: the work-graph section of backends/d3d12/Pipeline.cpp.

#include "DeviceState.hpp"

#include <RI/Error.hpp>

#include <string>

namespace RI::Vk {

namespace {

[[noreturn]] void unsupported(const char *call) {
    reportError(ErrorCategory::eUnsupported,
                std::string(call) + ": work graphs are D3D12 only (P50; DeviceLimits::workGraphs is false on Vulkan)");
}

} // namespace

WorkGraph createWorkGraph(Device &, const WorkGraphDesc &) { unsupported("createWorkGraph"); }
void destroyWorkGraph(Device &, WorkGraph &) { unsupported("destroyWorkGraph"); }
uint32 workGraphEntry(const WorkGraph &, std::string_view, uint32) { unsupported("workGraphEntry"); }
void bindWorkGraphMemory(Device &, WorkGraph &, const Buffer &, uint64, uint64) { unsupported("bindWorkGraphMemory"); }
void cmdSetWorkGraph(CommandList &, const WorkGraph &) { unsupported("cmdSetWorkGraph"); }
void cmdDispatchGraph(CommandList &, std::span<const GraphInput>) { unsupported("cmdDispatchGraph"); }
void cmdDispatchGraphIndirect(CommandList &, uint64, bool) { unsupported("cmdDispatchGraphIndirect"); }

} // namespace RI::Vk
