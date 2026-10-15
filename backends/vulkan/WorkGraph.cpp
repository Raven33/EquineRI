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
void destroyWorkGraph([[maybe_unused]] Device &device, WorkGraph &) {
    EQ_VK_CHECK_THREAD(device.internal.state);
    unsupported("destroyWorkGraph");
}
uint32 workGraphEntry(const WorkGraph &, std::string_view, uint32) { unsupported("workGraphEntry"); }
void bindWorkGraphMemory([[maybe_unused]] Device &device, WorkGraph &, const Buffer &, uint64, uint64) {
    EQ_VK_CHECK_THREAD(device.internal.state);
    unsupported("bindWorkGraphMemory");
}
void cmdSetWorkGraph([[maybe_unused]] CommandList &cmd, const WorkGraph &) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    unsupported("cmdSetWorkGraph");
}
void cmdDispatchGraph([[maybe_unused]] CommandList &cmd, std::span<const GraphInput>) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    unsupported("cmdDispatchGraph");
}
void cmdDispatchGraphIndirect([[maybe_unused]] CommandList &cmd, uint64, bool) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    unsupported("cmdDispatchGraphIndirect");
}

} // namespace RI::Vk
