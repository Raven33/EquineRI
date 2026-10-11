// Canonical Vulkan: object names and command labels with VK_EXT_debug_utils
// (an instance extension; its functions are fetched from the instance).
#undef VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <cstdint>

// --8<-- [start:names]
void nameBuffer(VkInstance instance, VkDevice device, VkBuffer buffer, const char *name) {
    const auto setName = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
        vkGetInstanceProcAddr(instance, "vkSetDebugUtilsObjectNameEXT"));
    const VkDebugUtilsObjectNameInfoEXT info{.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
                                             .objectType = VK_OBJECT_TYPE_BUFFER,
                                             .objectHandle = reinterpret_cast<uint64_t>(buffer),
                                             .pObjectName = name};
    setName(device, &info);
}
// --8<-- [end:names]

// --8<-- [start:markers]
void labelled(VkInstance instance, VkCommandBuffer commands) {
    const auto begin = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
        vkGetInstanceProcAddr(instance, "vkCmdBeginDebugUtilsLabelEXT"));
    const auto end = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
        vkGetInstanceProcAddr(instance, "vkCmdEndDebugUtilsLabelEXT"));
    const VkDebugUtilsLabelEXT label{.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT, .pLabelName = "particles"};
    begin(commands, &label);
    // ... commands ...
    end(commands);
}
// --8<-- [end:markers]
