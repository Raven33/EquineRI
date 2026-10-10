#include "VkCheck.hpp"

#include <vulkan/vk_enum_string_helper.h>

#include <format>

namespace RI::Vk {

ErrorCategory categoryOf(VkResult result) {
    switch (result) {
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
    case VK_ERROR_OUT_OF_POOL_MEMORY:
    case VK_ERROR_FRAGMENTED_POOL:
    case VK_ERROR_FRAGMENTATION:
        return ErrorCategory::eOutOfMemory;
    case VK_ERROR_DEVICE_LOST:
        return ErrorCategory::eDeviceLost;
    case VK_ERROR_OUT_OF_DATE_KHR:
        return ErrorCategory::eSwapchainOutOfDate;
    case VK_ERROR_INITIALIZATION_FAILED:
    case VK_ERROR_LAYER_NOT_PRESENT:
    case VK_ERROR_EXTENSION_NOT_PRESENT:
    case VK_ERROR_FEATURE_NOT_PRESENT:
    case VK_ERROR_INCOMPATIBLE_DRIVER:
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
        return ErrorCategory::eUnsupported;
    default:
        return ErrorCategory::eProgrammerError;
    }
}

void checkVk(VkResult result, std::string_view call, std::source_location where) {
    if (result >= 0) {
        return;
    }
    reportError(categoryOf(result), std::format("{} failed: {}", call, string_VkResult(result)), where);
}

} // namespace RI::Vk
