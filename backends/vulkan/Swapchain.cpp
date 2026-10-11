// Swapchains on Vulkan (P43, P63, P64), in the D3D12 model: acquire() returns
// the screen texture, present() shows it.
//
// Owns: surfaces (Win32, Xlib, Wayland, through mirrored create-info structs),
// surface formats and present modes, create / resize / destroy, acquire and
// present.
//
// Keeps: per image, prerecorded command buffers for the layout switches and
// the semaphores RI hides; acquire waits on the acquire semaphore and moves
// the image to its resting layout (contents undefined), present moves it to
// PRESENT_SRC and signals. Both run on the graphics queue's timeline.
//
// Watch out: resize and destroy wait for the queue to be idle (present
// semaphores may still be in use).
//
// D3D12 counterpart: backends/d3d12/Swapchain.cpp.

#include "DeviceState.hpp"
#include "VkCheck.hpp"

#include <RI/Error.hpp>

#include <algorithm>
#include <format>
#include <vector>

namespace RI::Vk {

struct SwapchainState {
    SwapchainDesc desc;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    std::vector<Texture> textures;            // one per image
    std::vector<VkCommandBuffer> toRest;      // per image: UNDEFINED -> resting
    std::vector<VkCommandBuffer> toPresent;   // per image: resting -> PRESENT_SRC
    std::vector<VkSemaphore> presentReady;    // per image, waited by vkQueuePresentKHR
    std::vector<uint64> imageUse;             // per image: timeline value of its last present submit
    std::vector<VkSemaphore> acquired;        // imageCount + 1, rotating
    std::vector<uint64> acquiredUse;          // timeline value of the submit that waited on each
    uint32 nextAcquired = 0;
    uint32 current = UINT32_MAX;              // the acquired image, until present
};

namespace {

// Surface-creation structures, mirrored so RI needs no window-system headers
// (each matches the Vulkan header's VkXxxSurfaceCreateInfoKHR).
struct XlibSurfaceInfo {
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    void *display;
    unsigned long window;
};
struct XcbSurfaceInfo {
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    void *connection;
    uint32_t window;
};
struct WaylandSurfaceInfo {
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    void *display;
    void *surface;
};
struct Win32SurfaceInfo {
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    void *hinstance;
    void *hwnd;
};
using CreateSurface = VkResult(VKAPI_PTR *)(VkInstance, const void *, const VkAllocationCallbacks *, VkSurfaceKHR *);

template <typename T>
T instanceFunction(const DeviceState &state, const char *name) {
    return reinterpret_cast<T>(vkGetInstanceProcAddr(state.instance, name));
}

VkSurfaceKHR createSurface(const DeviceState &state, const Window &window) {
    const char *name = nullptr;
    const void *info = nullptr;
    XlibSurfaceInfo xlib{};
    XcbSurfaceInfo xcb{};
    WaylandSurfaceInfo wayland{};
    Win32SurfaceInfo win32{};
    switch (window.system) {
    case WindowSystem::eXlib:
        xlib = {VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR, nullptr, 0, window.display,
                static_cast<unsigned long>(window.id)};
        name = "vkCreateXlibSurfaceKHR";
        info = &xlib;
        break;
    case WindowSystem::eXcb:
        xcb = {VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR, nullptr, 0, window.display,
               static_cast<uint32_t>(window.id)};
        name = "vkCreateXcbSurfaceKHR";
        info = &xcb;
        break;
    case WindowSystem::eWayland:
        wayland = {VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR, nullptr, 0, window.display, window.handle};
        name = "vkCreateWaylandSurfaceKHR";
        info = &wayland;
        break;
    case WindowSystem::eWin32:
        win32 = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, nullptr, 0, window.display, window.handle};
        name = "vkCreateWin32SurfaceKHR";
        info = &win32;
        break;
    }
    const auto create = instanceFunction<CreateSurface>(state, name);
    if (create == nullptr) {
        reportError(ErrorCategory::eUnsupported,
                    std::format("createSwapchain: the Vulkan loader has no {} (window system not available)", name));
    }
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    checkVk(create(state.instance, info, nullptr, &surface), name);
    const auto supported = instanceFunction<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(
        state, "vkGetPhysicalDeviceSurfaceSupportKHR");
    VkBool32 presentable = VK_FALSE;
    checkVk(supported(state.physicalDevice, state.graphicsQueueFamily, surface, &presentable),
            "vkGetPhysicalDeviceSurfaceSupportKHR");
    if (presentable != VK_TRUE) {
        instanceFunction<PFN_vkDestroySurfaceKHR>(state, "vkDestroySurfaceKHR")(state.instance, surface, nullptr);
        reportError(ErrorCategory::eUnsupported, "createSwapchain: the graphics queue cannot present to this window");
    }
    return surface;
}

void destroySurface(const DeviceState &state, VkSurfaceKHR surface) {
    instanceFunction<PFN_vkDestroySurfaceKHR>(state, "vkDestroySurfaceKHR")(state.instance, surface, nullptr);
}

VkColorSpaceKHR colorSpaceOf(ColorSpace space) {
    switch (space) {
    case ColorSpace::eSrgb: return VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    case ColorSpace::eHdr10: return VK_COLOR_SPACE_HDR10_ST2084_EXT;
    case ColorSpace::eScRgb: return VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
    }
    return VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
}

VkPresentModeKHR presentModeOf(PresentMode mode) {
    switch (mode) {
    case PresentMode::eFifo: return VK_PRESENT_MODE_FIFO_KHR;
    case PresentMode::eMailbox: return VK_PRESENT_MODE_MAILBOX_KHR;
    case PresentMode::eImmediate: return VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

std::vector<VkSurfaceFormatKHR> nativeFormats(const DeviceState &state, VkSurfaceKHR surface) {
    const auto get = instanceFunction<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(
        state, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    uint32 count = 0;
    checkVk(get(state.physicalDevice, surface, &count, nullptr), "vkGetPhysicalDeviceSurfaceFormatsKHR");
    std::vector<VkSurfaceFormatKHR> formats(count);
    checkVk(get(state.physicalDevice, surface, &count, formats.data()), "vkGetPhysicalDeviceSurfaceFormatsKHR");
    return formats;
}

std::vector<VkPresentModeKHR> nativeModes(const DeviceState &state, VkSurfaceKHR surface) {
    const auto get = instanceFunction<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(
        state, "vkGetPhysicalDeviceSurfacePresentModesKHR");
    uint32 count = 0;
    checkVk(get(state.physicalDevice, surface, &count, nullptr), "vkGetPhysicalDeviceSurfacePresentModesKHR");
    std::vector<VkPresentModeKHR> modes(count);
    checkVk(get(state.physicalDevice, surface, &count, modes.data()), "vkGetPhysicalDeviceSurfacePresentModesKHR");
    return modes;
}

// The RI formats a swapchain may use.
constexpr Format kScreenFormats[] = {Format::eBGRA8Unorm, Format::eBGRA8Srgb,     Format::eRGBA8Unorm,
                                     Format::eRGBA8Srgb,  Format::eRGB10A2Unorm, Format::eRGBA16Float};

TextureUsage screenUsage(bool storage) {
    return TextureUsage::eRenderTarget | TextureUsage::eCopyDst | (storage ? TextureUsage::eStorage : TextureUsage::eNone);
}

void waitTimeline(DeviceState &state, uint64 value) {
    if (value == 0) {
        return;
    }
    const VkSemaphoreWaitInfo info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                   .semaphoreCount = 1,
                                   .pSemaphores = &state.timeline,
                                   .pValues = &value};
    checkQueueVk(state, state.functions.vkWaitSemaphores(state.device, &info, UINT64_MAX), "vkWaitSemaphores");
}

// Submits `commands` waiting on `wait` (if any) and signalling `signal` (if
// any) and the next timeline value, which it returns.
uint64 submitInternalCommands(DeviceState &state, VkCommandBuffer commands, VkSemaphore wait, VkSemaphore signal) {
    const uint64 value = state.lastSignaled + 1;
    const VkCommandBufferSubmitInfo commandInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                                .commandBuffer = commands};
    const VkSemaphoreSubmitInfo waitInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                         .semaphore = wait,
                                         .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    const VkSemaphoreSubmitInfo signals[2] = {
        {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
         .semaphore = state.timeline,
         .value = value,
         .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
        {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
         .semaphore = signal,
         .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
    };
    const VkSubmitInfo2 submitInfo{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                                   .waitSemaphoreInfoCount = wait != VK_NULL_HANDLE ? 1u : 0u,
                                   .pWaitSemaphoreInfos = &waitInfo,
                                   .commandBufferInfoCount = 1,
                                   .pCommandBufferInfos = &commandInfo,
                                   .signalSemaphoreInfoCount = signal != VK_NULL_HANDLE ? 2u : 1u,
                                   .pSignalSemaphoreInfos = signals};
    checkQueueVk(state, state.functions.vkQueueSubmit2(state.graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE),
                 "vkQueueSubmit2");
    state.lastSignaled = value;
    return value;
}

void recordSwitch(DeviceState &state, VkCommandBuffer commands, const TextureView &view, VkImageLayout from,
                  VkImageLayout to, VkAccessFlags2 srcAccess) {
    const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    checkVk(state.functions.vkBeginCommandBuffer(commands, &begin), "vkBeginCommandBuffer");
    const VkImageMemoryBarrier2 barrier = imageBarrier(view, {.srcStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                                              .srcAccess = srcAccess,
                                                              .dstStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                                              .dstAccess = VK_ACCESS_2_MEMORY_READ_BIT |
                                                                           VK_ACCESS_2_MEMORY_WRITE_BIT,
                                                              .oldLayout = from,
                                                              .newLayout = to});
    const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                      .imageMemoryBarrierCount = 1,
                                      .pImageMemoryBarriers = &barrier};
    state.functions.vkCmdPipelineBarrier2(commands, &dependency);
    checkVk(state.functions.vkEndCommandBuffer(commands), "vkEndCommandBuffer");
}

void releaseImages(DeviceState &state, SwapchainState &s) {
    for (Texture &texture : s.textures) {
        EQ_VK_UNTRACK(&state, texture.internal.id);
        state.functions.vkDestroyImageView(state.device, reinterpret_cast<VkImageView>(texture.view.internal.view),
                                           nullptr);
    }
    s.textures.clear();
    if (!s.toRest.empty()) {
        state.functions.vkFreeCommandBuffers(state.device, s.pool, static_cast<uint32>(s.toRest.size()), s.toRest.data());
        state.functions.vkFreeCommandBuffers(state.device, s.pool, static_cast<uint32>(s.toPresent.size()),
                                             s.toPresent.data());
    }
    s.toRest.clear();
    s.toPresent.clear();
    for (VkSemaphore semaphore : s.presentReady) {
        state.functions.vkDestroySemaphore(state.device, semaphore, nullptr);
    }
    for (VkSemaphore semaphore : s.acquired) {
        state.functions.vkDestroySemaphore(state.device, semaphore, nullptr);
    }
    s.presentReady.clear();
    s.acquired.clear();
    s.imageUse.clear();
    s.acquiredUse.clear();
}

// (Re)creates the native swapchain and RI's per-image objects for `swapchain`.
void build(DeviceState &state, Swapchain &swapchain, uint32 width, uint32 height) {
    SwapchainState &s = *swapchain.internal.state;
    const auto capabilitiesOf = instanceFunction<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
        state, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    VkSurfaceCapabilitiesKHR caps{};
    checkVk(capabilitiesOf(state.physicalDevice, s.surface, &caps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    // The window decides the size where it can (X11, Win32); Wayland lets the swapchain decide.
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent = {std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width),
                  std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height)};
    }
    uint32 imageCount = s.desc.imageCount != 0 ? s.desc.imageCount : caps.minImageCount + 1;
    imageCount = std::max(imageCount, caps.minImageCount);
    if (caps.maxImageCount != 0) {
        imageCount = std::min(imageCount, caps.maxImageCount);
    }
    const TextureUsage usage = screenUsage(s.desc.storage);
    const VkSwapchainKHR old = s.swapchain;
    const VkSwapchainCreateInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = s.surface,
        .minImageCount = imageCount,
        .imageFormat = toVkFormat(s.desc.format.format),
        .imageColorSpace = colorSpaceOf(s.desc.format.colorSpace),
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = imageUsageOf(usage),
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) != 0
                              ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                              : static_cast<VkCompositeAlphaFlagBitsKHR>(caps.supportedCompositeAlpha &
                                                                         -caps.supportedCompositeAlpha),
        .presentMode = presentModeOf(s.desc.presentMode),
        .clipped = VK_TRUE,
        .oldSwapchain = old,
    };
    checkVk(state.functions.vkCreateSwapchainKHR(state.device, &info, nullptr, &s.swapchain), "vkCreateSwapchainKHR");
    if (old != VK_NULL_HANDLE) {
        state.functions.vkDestroySwapchainKHR(state.device, old, nullptr);
    }
    uint32 count = 0;
    checkVk(state.functions.vkGetSwapchainImagesKHR(state.device, s.swapchain, &count, nullptr), "vkGetSwapchainImagesKHR");
    std::vector<VkImage> images(count);
    checkVk(state.functions.vkGetSwapchainImagesKHR(state.device, s.swapchain, &count, images.data()),
            "vkGetSwapchainImagesKHR");

    s.toRest.resize(count);
    s.toPresent.resize(count);
    const VkCommandBufferAllocateInfo allocInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                .commandPool = s.pool,
                                                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                .commandBufferCount = count};
    checkVk(state.functions.vkAllocateCommandBuffers(state.device, &allocInfo, s.toRest.data()), "vkAllocateCommandBuffers");
    checkVk(state.functions.vkAllocateCommandBuffers(state.device, &allocInfo, s.toPresent.data()),
            "vkAllocateCommandBuffers");
    const VkSemaphoreCreateInfo semaphoreInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    const VkImageLayout resting = restingLayoutOf(state.layoutMode, s.desc.format.format, usage);
    Device device{.internal = {.state = &state}};
    for (uint32 i = 0; i < count; ++i) {
        Texture texture;
        texture.width = extent.width;
        texture.height = extent.height;
        texture.format = s.desc.format.format;
        texture.usage = usage;
        texture.internal.image = reinterpret_cast<uint64>(images[i]);
        texture.internal.restingLayout = static_cast<uint32>(resting);
        texture.internal.id = EQ_VK_TRACK(&state);
        texture.view = createView(device, texture, {});
        s.textures.push_back(texture);
        recordSwitch(state, s.toRest[i], texture.view, VK_IMAGE_LAYOUT_UNDEFINED, resting, VK_ACCESS_2_NONE);
        recordSwitch(state, s.toPresent[i], texture.view, resting, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     VK_ACCESS_2_MEMORY_WRITE_BIT);
        VkSemaphore semaphore = VK_NULL_HANDLE;
        checkVk(state.functions.vkCreateSemaphore(state.device, &semaphoreInfo, nullptr, &semaphore), "vkCreateSemaphore");
        s.presentReady.push_back(semaphore);
    }
    for (uint32 i = 0; i < count + 1; ++i) {
        VkSemaphore semaphore = VK_NULL_HANDLE;
        checkVk(state.functions.vkCreateSemaphore(state.device, &semaphoreInfo, nullptr, &semaphore), "vkCreateSemaphore");
        s.acquired.push_back(semaphore);
    }
    s.imageUse.assign(count, 0);
    s.acquiredUse.assign(count + 1, 0);
    s.nextAcquired = 0;
    s.current = UINT32_MAX;
    swapchain.width = extent.width;
    swapchain.height = extent.height;
    swapchain.imageCount = count;
}

} // namespace

std::vector<SurfaceFormat> surfaceFormats(Device &device, const Window &window) {
    DeviceState &state = *device.internal.state;
    const VkSurfaceKHR surface = createSurface(state, window);
    std::vector<SurfaceFormat> result;
    for (const VkSurfaceFormatKHR &native : nativeFormats(state, surface)) {
        for (Format format : kScreenFormats) {
            for (ColorSpace space : {ColorSpace::eSrgb, ColorSpace::eHdr10, ColorSpace::eScRgb}) {
                if (native.format == toVkFormat(format) && native.colorSpace == colorSpaceOf(space)) {
                    result.push_back({format, space});
                }
            }
        }
    }
    destroySurface(state, surface);
    return result;
}

bool presentModeSupported(Device &device, const Window &window, PresentMode mode) {
    DeviceState &state = *device.internal.state;
    const VkSurfaceKHR surface = createSurface(state, window);
    const std::vector<VkPresentModeKHR> modes = nativeModes(state, surface);
    destroySurface(state, surface);
    return std::ranges::find(modes, presentModeOf(mode)) != modes.end();
}

bool screenStorageSupported(Device &device, const Window &window, Format format) {
    DeviceState &state = *device.internal.state;
    const VkSurfaceKHR surface = createSurface(state, window);
    VkSurfaceCapabilitiesKHR caps{};
    checkVk(instanceFunction<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
                state, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")(state.physicalDevice, surface, &caps),
            "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    destroySurface(state, surface);
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(state.physicalDevice, toVkFormat(format), &properties);
    return (caps.supportedUsageFlags & VK_IMAGE_USAGE_STORAGE_BIT) != 0 &&
           (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
}

Swapchain createSwapchain(Device &device, const SwapchainDesc &desc) {
    DeviceState &state = *device.internal.state;
    auto *s = new SwapchainState;
    s->desc = desc;
    s->surface = createSurface(state, desc.window);
    const std::vector<VkSurfaceFormatKHR> formats = nativeFormats(state, s->surface);
    const bool formatOk = std::ranges::any_of(formats, [&](const VkSurfaceFormatKHR &f) {
        return f.format == toVkFormat(desc.format.format) && f.colorSpace == colorSpaceOf(desc.format.colorSpace);
    });
    const std::vector<VkPresentModeKHR> modes = nativeModes(state, s->surface);
    if (!formatOk || std::ranges::find(modes, presentModeOf(desc.presentMode)) == modes.end()) {
        destroySurface(state, s->surface);
        delete s;
        reportError(ErrorCategory::eUnsupported,
                    "createSwapchain: this window does not support the format, color space or present mode; ask "
                    "surfaceFormats and presentModeSupported first (P64)");
    }
    const VkCommandPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                           .queueFamilyIndex = state.graphicsQueueFamily};
    checkVk(state.functions.vkCreateCommandPool(state.device, &poolInfo, nullptr, &s->pool), "vkCreateCommandPool");
    Swapchain swapchain;
    swapchain.format = desc.format;
    swapchain.presentMode = desc.presentMode;
    swapchain.internal.state = s;
    swapchain.internal.id = EQ_VK_TRACK(&state);
    build(state, swapchain, desc.width, desc.height);
    return swapchain;
}

void resizeSwapchain(Device &device, Swapchain &swapchain, uint32 width, uint32 height) {
    DeviceState &state = *device.internal.state;
    EQ_VK_CHECK_ALIVE(&state, swapchain.internal.id, "swapchain");
    // No image or semaphore in use: the timeline covers submissions, the
    // queue wait also covers presentations (which take no fence).
    checkQueueVk(state, state.functions.vkQueueWaitIdle(state.graphicsQueue), "vkQueueWaitIdle");
    releaseImages(state, *swapchain.internal.state);
    build(state, swapchain, width, height);
}

void destroySwapchain(Device &device, Swapchain &swapchain) {
    DeviceState &state = *device.internal.state;
    EQ_VK_CHECK_ALIVE(&state, swapchain.internal.id, "swapchain");
    EQ_VK_UNTRACK(&state, swapchain.internal.id);
    SwapchainState *s = swapchain.internal.state;
    checkQueueVk(state, state.functions.vkQueueWaitIdle(state.graphicsQueue), "vkQueueWaitIdle");
    releaseImages(state, *s);
    state.functions.vkDestroyCommandPool(state.device, s->pool, nullptr);
    state.functions.vkDestroySwapchainKHR(state.device, s->swapchain, nullptr);
    destroySurface(state, s->surface);
    delete s;
    swapchain = {};
}

Texture acquire(Device &device, Swapchain &swapchain) {
    DeviceState &state = *device.internal.state;
    EQ_VK_CHECK_ALIVE(&state, swapchain.internal.id, "swapchain");
    SwapchainState &s = *swapchain.internal.state;
    if (s.current != UINT32_MAX) {
        reportError(ErrorCategory::eProgrammerError, "acquire: the screen texture of this frame was not presented yet");
    }
    // The semaphore's previous waiter must be done before it is signalled again.
    const uint32 slot = s.nextAcquired;
    s.nextAcquired = (s.nextAcquired + 1) % static_cast<uint32>(s.acquired.size());
    waitTimeline(state, s.acquiredUse[slot]);
    uint32 index = 0;
    const VkResult result = state.functions.vkAcquireNextImageKHR(state.device, s.swapchain, UINT64_MAX,
                                                                  s.acquired[slot], VK_NULL_HANDLE, &index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        reportError(ErrorCategory::eSwapchainOutOfDate,
                    "acquire: the swapchain no longer matches the window; call resizeSwapchain");
        return {};
    }
    if (result != VK_SUBOPTIMAL_KHR) {
        checkQueueVk(state, result, "vkAcquireNextImageKHR");
    }
    // The image's command buffers from its last frame must be done.
    waitTimeline(state, s.imageUse[index]);
    s.acquiredUse[slot] = submitInternalCommands(state, s.toRest[index], s.acquired[slot], VK_NULL_HANDLE);
    s.current = index;
    return s.textures[index];
}

void present(Queue &queue, Swapchain &swapchain) {
    DeviceState &state = *queue.internal.device;
    EQ_VK_CHECK_ALIVE(&state, swapchain.internal.id, "swapchain");
    SwapchainState &s = *swapchain.internal.state;
    if (s.current == UINT32_MAX) {
        reportError(ErrorCategory::eProgrammerError, "present: no screen texture was acquired");
    }
    const uint32 index = s.current;
    s.current = UINT32_MAX;
    s.imageUse[index] = submitInternalCommands(state, s.toPresent[index], VK_NULL_HANDLE, s.presentReady[index]);
    const VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                .waitSemaphoreCount = 1,
                                .pWaitSemaphores = &s.presentReady[index],
                                .swapchainCount = 1,
                                .pSwapchains = &s.swapchain,
                                .pImageIndices = &index};
    const VkResult result = state.functions.vkQueuePresentKHR(state.graphicsQueue, &info);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        reportError(ErrorCategory::eSwapchainOutOfDate,
                    "present: the swapchain no longer matches the window; call resizeSwapchain");
        return;
    }
    if (result != VK_SUBOPTIMAL_KHR) {
        checkQueueVk(state, result, "vkQueuePresentKHR");
    }
}

} // namespace RI::Vk
