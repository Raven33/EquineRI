// Canonical Vulkan: how errors are reported without RI. Every call returns a
// VkResult that you check yourself; validation-layer messages arrive through a
// debug messenger callback.
//
// A stand-alone program linking the Vulkan loader (prototypes turned back on,
// see snippets/vulkan/device.cpp).
#undef VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>

// --8<-- [start:check-result]
// Negative VkResult values are errors; positive ones (VK_INCOMPLETE, ...) are
// successes with extra information.
void check(VkResult result, const char *call) {
    if (result < 0) {
        std::fprintf(stderr, "%s failed: VkResult %d\n", call, static_cast<int>(result));
        std::abort();
    }
}

// The validation layer reports mistakes here, not through VkResult.
VKAPI_ATTR VkBool32 VKAPI_CALL onMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                         VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                         const VkDebugUtilsMessengerCallbackDataEXT *data, void * /*user*/) {
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        std::fprintf(stderr, "validation error: %s\n", data->pMessage);
    }
    return VK_FALSE; // never VK_TRUE: that would make the call fail
}
// --8<-- [end:check-result]

int main() {
    // --8<-- [start:messenger]
    // Passing the messenger's create info in the instance's pNext also catches
    // messages from vkCreateInstance itself.
    const VkDebugUtilsMessengerCreateInfoEXT messengerInfo{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
        .pfnUserCallback = &onMessage,
    };
    const char *const layers[] = {"VK_LAYER_KHRONOS_validation"};
    const char *const extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    const VkApplicationInfo appInfo{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    const VkInstanceCreateInfo instanceInfo{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = &messengerInfo,
        .pApplicationInfo = &appInfo,
        .enabledLayerCount = 1,
        .ppEnabledLayerNames = layers,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = extensions,
    };
    VkInstance instance = VK_NULL_HANDLE;
    check(vkCreateInstance(&instanceInfo, nullptr, &instance), "vkCreateInstance");
    // --8<-- [end:messenger]
    vkDestroyInstance(instance, nullptr);
    return 0;
}
