// Debugging (P53): object names and command markers (always on, through
// VK_EXT_debug_utils), and GPU crash details from VK_EXT_device_fault when a
// validation build loses the device.

#include "DeviceState.hpp"
#include "VkCheck.hpp"

#include <RI/Error.hpp>
#if EQUINE_VALIDATION
#include <RI/Validation/Checks.hpp>
#endif

#include <format>
#include <string>
#include <vector>

namespace RI::Vk {

void loadDebugFunctions(DeviceState &state) {
    state.setObjectName = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
        vkGetInstanceProcAddr(state.instance, "vkSetDebugUtilsObjectNameEXT"));
    state.beginLabel = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
        vkGetInstanceProcAddr(state.instance, "vkCmdBeginDebugUtilsLabelEXT"));
    state.endLabel = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
        vkGetInstanceProcAddr(state.instance, "vkCmdEndDebugUtilsLabelEXT"));
}

namespace {

void nameObject(Device &device, VkObjectType type, uint64 handle, std::string_view name) {
    const DeviceState &state = *device.internal.state;
    if (handle == 0 || state.setObjectName == nullptr) {
        return;
    }
    const std::string text(name);
    const VkDebugUtilsObjectNameInfoEXT info{.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
                                             .objectType = type,
                                             .objectHandle = handle,
                                             .pObjectName = text.c_str()};
    checkVk(state.setObjectName(state.device, &info), "vkSetDebugUtilsObjectNameEXT");
}

} // namespace

void setName(Device &device, const Memory &memory, std::string_view name) {
    nameObject(device, VK_OBJECT_TYPE_DEVICE_MEMORY, memory.internal.memory, name);
}
void setName(Device &device, const Buffer &buffer, std::string_view name) {
    nameObject(device, VK_OBJECT_TYPE_BUFFER, buffer.internal.buffer, name);
}
void setName(Device &device, const Texture &texture, std::string_view name) {
    nameObject(device, VK_OBJECT_TYPE_IMAGE, texture.internal.image, name);
    nameObject(device, VK_OBJECT_TYPE_IMAGE_VIEW, texture.view.internal.view, name);
}
void setName(Device &device, const TextureView &view, std::string_view name) {
    nameObject(device, VK_OBJECT_TYPE_IMAGE_VIEW, view.internal.view, name);
}
void setName(Device &device, const DescriptorHeap &heap, std::string_view name) {
    nameObject(device, VK_OBJECT_TYPE_DESCRIPTOR_POOL, heap.internal.pool, name);
    nameObject(device, VK_OBJECT_TYPE_DESCRIPTOR_SET, heap.internal.resourceSet, name);
    nameObject(device, VK_OBJECT_TYPE_DESCRIPTOR_SET, heap.internal.samplerSet, name);
}
void setName(Device &device, const QueryPool &pool, std::string_view name) {
    nameObject(device, VK_OBJECT_TYPE_QUERY_POOL, pool.internal.pool, name);
}
void setName(Device &device, const Pipeline &pipeline, std::string_view name) {
    nameObject(device, VK_OBJECT_TYPE_PIPELINE, pipeline.internal.pipeline, name);
}
void setName(Device &device, const CommandList &cmd, std::string_view name) {
    nameObject(device, VK_OBJECT_TYPE_COMMAND_BUFFER, reinterpret_cast<uint64>(cmd.internal.commandBuffer), name);
}

void cmdBeginMarker(CommandList &cmd, std::string_view name) {
    const DeviceState &state = *cmd.internal.device;
    ++cmd.internal.markerDepth;
    if (state.beginLabel == nullptr) {
        return;
    }
    const std::string text(name);
    const VkDebugUtilsLabelEXT label{.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT, .pLabelName = text.c_str()};
    state.beginLabel(static_cast<VkCommandBuffer>(cmd.internal.commandBuffer), &label);
}

void cmdEndMarker(CommandList &cmd) {
    EQ_VALIDATE(Validation::checkMarkerEnd(cmd.internal.markerDepth));
    --cmd.internal.markerDepth;
    const DeviceState &state = *cmd.internal.device;
    if (state.endLabel != nullptr) {
        state.endLabel(static_cast<VkCommandBuffer>(cmd.internal.commandBuffer));
    }
}

// P53 (2): what VK_EXT_device_fault reports about a lost device, or "".
std::string deviceFaultDetails(DeviceState &state) {
    if (!state.faultEnabled || state.functions.vkGetDeviceFaultInfoEXT == nullptr) {
        return {};
    }
    VkDeviceFaultCountsEXT counts{.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    if (state.functions.vkGetDeviceFaultInfoEXT(state.device, &counts, nullptr) != VK_SUCCESS) {
        return {};
    }
    std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
    std::vector<VkDeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
    counts.vendorBinarySize = 0; // not read
    VkDeviceFaultInfoEXT info{.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT,
                              .pAddressInfos = addresses.data(),
                              .pVendorInfos = vendors.data()};
    if (state.functions.vkGetDeviceFaultInfoEXT(state.device, &counts, &info) != VK_SUCCESS) {
        return {};
    }
    std::string text = std::format("; device fault: {}", info.description);
    for (const VkDeviceFaultAddressInfoEXT &a : addresses) {
        text += std::format("; address 0x{:x} (+/- 0x{:x}, type {})", a.reportedAddress, a.addressPrecision,
                            static_cast<int>(a.addressType));
    }
    for (const VkDeviceFaultVendorInfoEXT &v : vendors) {
        text += std::format("; vendor: {} (code 0x{:x}, data 0x{:x})", v.description, v.vendorFaultCode,
                            v.vendorFaultData);
    }
    return text;
}

void checkQueueVk(DeviceState &state, VkResult result, std::string_view call) {
    if (result == VK_ERROR_DEVICE_LOST) {
        reportError(ErrorCategory::eDeviceLost,
                    std::format("{} failed: VK_ERROR_DEVICE_LOST{}", call, deviceFaultDetails(state)));
    }
    checkVk(result, call);
}

} // namespace RI::Vk
