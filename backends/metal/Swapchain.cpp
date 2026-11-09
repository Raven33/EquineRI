// Swapchains on Metal (P43, P63, P64): a CAMetalLayer on the window's NSView.
//
// Owns: surface formats and present modes, create / resize / destroy, acquire
// and present. createSwapchain gives the view a CAMetalLayer (or keeps the one
// it has) and configures it; acquire takes the layer's next drawable, which
// blocks while none is free, and makes the queue wait for it before the work
// committed after (MTL4::CommandQueue::wait); present signals the drawable
// after the work committed so far and presents it. Neither submits work:
// Metal has no layouts. The layer's residency set joins the queue, so its
// drawables need no residency of RI's.
//
// Watch out: a drawable is new at each acquire, but its texture comes from
// the layer's small pool: each texture is wrapped once as an RI::Texture (its
// own view, no newTextureView) and kept until resize or destroy, which wait
// for the GPU first. The layer and the screen's EDR headroom are AppKit
// objects: call surfaceFormats and createSwapchain on the thread that owns
// the window (the main thread). The layer is not framebuffer-only, so the
// screen texture takes copies (eCopyDst) and, where the format allows, compute
// writes. The view keeps its layer after destroySwapchain.
//
// Vulkan counterpart: backends/vulkan/Swapchain.cpp; D3D12: backends/d3d12/Swapchain.cpp.

#include "DeviceState.hpp"

#include <RI/Error.hpp>

#include <QuartzCore/QuartzCore.hpp>

#include <CoreGraphics/CoreGraphics.h>
#include <objc/message.h>
#include <objc/runtime.h>

#include <algorithm>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace RI::Metal {

struct SwapchainState {
    SwapchainDesc desc;               // without its name, which `name` keeps
    std::string name;                 // names the textures, also those wrapped after a resize (P106)
    CA::MetalLayer *layer = nullptr;  // retained
    // The drawable textures met so far (retained), each with its RI::Texture.
    std::vector<std::pair<MTL::Texture *, Texture>> textures;
    CA::MetalDrawable *current = nullptr; // retained from acquire to present
};

namespace {

// Drawables, the layer and AppKit's objects are autoreleased.
struct ScopedPool {
    NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();
    ScopedPool() = default;
    ScopedPool(const ScopedPool &) = delete;
    ScopedPool &operator=(const ScopedPool &) = delete;
    ~ScopedPool() { pool->release(); }
};

// An Objective-C message to an AppKit object (metal-cpp wraps none of AppKit).
template <typename R, typename... Args>
R send(const void *object, const char *selector, Args... args) {
    using Message = R (*)(const void *, SEL, Args...);
    return reinterpret_cast<Message>(&objc_msgSend)(object, sel_registerName(selector), args...);
}

void *viewOf(const Window &window, const char *call) {
    if (window.system != WindowSystem::eCocoa || window.handle == nullptr) {
        reportError(ErrorCategory::eUnsupported, std::format("{}: Metal presents to Cocoa windows (an NSView)", call));
    }
    return window.handle;
}

// The largest color value the view's screen can show (1 on SDR screens).
double edrHeadroom(void *view) {
    void *window = send<void *>(view, "window");
    void *screen = window != nullptr ? send<void *>(window, "screen") : nullptr;
    return screen != nullptr ? send<double>(screen, "maximumPotentialExtendedDynamicRangeColorComponentValue") : 1.0;
}

// The view's CAMetalLayer: the one it has, or a new one it hosts.
CA::MetalLayer *layerOf(void *view) {
    void *layer = send<void *>(view, "layer");
    if (layer != nullptr && send<bool>(layer, "isKindOfClass:", objc_lookUpClass("CAMetalLayer"))) {
        return static_cast<CA::MetalLayer *>(layer);
    }
    CA::MetalLayer *created = CA::MetalLayer::layer();
    send<void>(view, "setLayer:", created);
    send<void>(view, "setWantsLayer:", true);
    return created;
}

CFStringRef colorSpaceName(ColorSpace space) {
    switch (space) {
    case ColorSpace::eSrgb: return kCGColorSpaceSRGB;
    case ColorSpace::eHdr10: return kCGColorSpaceITUR_2100_PQ;
    case ColorSpace::eScRgb: return kCGColorSpaceExtendedLinearSRGB;
    }
    return kCGColorSpaceSRGB;
}

// The image count asked, as CAMetalLayer allows it (2 or 3).
uint32 drawableCount(uint32 asked) { return asked == 0 ? 3 : std::clamp(asked, 2u, 3u); }

TextureUsage screenUsage(const SwapchainDesc &desc) {
    const TextureUsage usage = TextureUsage::eRenderTarget | TextureUsage::eCopyDst;
    return desc.storage ? usage | TextureUsage::eStorage : usage;
}

Texture wrap([[maybe_unused]] DeviceState &state, SwapchainState &s, MTL::Texture *native) {
    native->retain();
    Texture texture;
    texture.width = static_cast<uint32>(native->width());
    texture.height = static_cast<uint32>(native->height());
    texture.format = s.desc.format.format;
    texture.usage = screenUsage(s.desc);
    texture.internal.texture = native;
    texture.internal.imported = true; // the layer's
    texture.internal.id = EQ_MTL_TRACK(&state);
    TextureView &view = texture.view;
    view.format = texture.format;
    view.width = texture.width;
    view.height = texture.height;
    view.internal.texture = native;
    view.internal.resource = native;
    view.internal.textureUsage = texture.usage;
    view.internal.textureId = texture.internal.id;
    if (!s.name.empty()) {
        ScopedPool scope;
        native->setLabel(nsString(std::format("{} [{}]", s.name, s.textures.size())));
        EQ_MTL_TRACK_NAME(&state, texture.internal.id, s.name);
    }
    return texture;
}

void waitIdle(Device &device) { wait(device.graphicsQueue, {device.internal.state->lastSignaled}); }

void releaseTextures([[maybe_unused]] DeviceState &state, SwapchainState &s) {
    for (auto &[native, texture] : s.textures) {
        EQ_MTL_UNTRACK(&state, texture.internal.id);
        native->release();
    }
    s.textures.clear();
    release(s.current);
}

} // namespace

std::vector<SurfaceFormat> surfaceFormats([[maybe_unused]] Device &device, const Window &window) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    void *view = viewOf(window, "surfaceFormats");
    std::vector<SurfaceFormat> formats = {
        {Format::eBGRA8Srgb, ColorSpace::eSrgb},
        {Format::eBGRA8Unorm, ColorSpace::eSrgb},
        {Format::eRGB10A2Unorm, ColorSpace::eSrgb},
        {Format::eRGBA16Float, ColorSpace::eScRgb},
    };
    ScopedPool scope;
    if (edrHeadroom(view) > 1.0) {
        formats.push_back({Format::eRGB10A2Unorm, ColorSpace::eHdr10});
    }
    return formats;
}

// eFifo waits for the display (displaySyncEnabled); eImmediate does not. No
// mailbox: a layer never replaces a frame waiting to be shown.
bool presentModeSupported([[maybe_unused]] Device &device, const Window &window, PresentMode mode) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    viewOf(window, "presentModeSupported");
    return mode != PresentMode::eMailbox;
}

// Drawable textures take shader writes in the formats that allow them (not sRGB).
bool screenStorageSupported(Device &device, const Window &window, Format format) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    viewOf(window, "screenStorageSupported");
    return format != Format::eBGRA8Srgb && formatSupported(device, format, TextureUsage::eStorage);
}

Swapchain createSwapchain(Device &device, const SwapchainDesc &desc) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    DeviceState &state = *device.internal.state;
    void *view = viewOf(desc.window, "createSwapchain");
    const std::vector<SurfaceFormat> formats = surfaceFormats(device, desc.window);
    if (std::ranges::find(formats, desc.format) == formats.end() || !presentModeSupported(device, desc.window, desc.presentMode)) {
        reportError(ErrorCategory::eUnsupported,
                    "createSwapchain: this window does not support the format, color space or present mode; ask "
                    "surfaceFormats and presentModeSupported first (P64)");
    }
    if (desc.storage && !screenStorageSupported(device, desc.window, desc.format.format)) {
        reportError(ErrorCategory::eUnsupported,
                    "createSwapchain: compute cannot write the screen texture in this format (P64 (5)); ask "
                    "screenStorageSupported first");
    }
    if (desc.width == 0 || desc.height == 0) {
        reportError(ErrorCategory::eProgrammerError, "createSwapchain: the width and height must not be 0");
    }

    ScopedPool scope;
    auto *s = new SwapchainState;
    s->desc = desc;
    s->name = desc.name;
    s->desc.name = {};
    s->layer = layerOf(view)->retain();
    CA::MetalLayer *layer = s->layer;
    layer->setDevice(state.device);
    layer->setPixelFormat(pixelFormatOf(desc.format.format));
    layer->setFramebufferOnly(false); // copies and compute writes (P64 (5))
    layer->setDrawableSize(CGSize{static_cast<double>(desc.width), static_cast<double>(desc.height)});
    layer->setMaximumDrawableCount(drawableCount(desc.imageCount));
    layer->setDisplaySyncEnabled(desc.presentMode == PresentMode::eFifo);
    layer->setWantsExtendedDynamicRangeContent(desc.format.colorSpace != ColorSpace::eSrgb);
    CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(colorSpaceName(desc.format.colorSpace));
    layer->setColorspace(colorSpace);
    CGColorSpaceRelease(colorSpace);
    // Pixels of the drawable map 1:1 to the screen on Retina displays.
    void *window = send<void *>(view, "window");
    if (window != nullptr) {
        send<void>(layer, "setContentsScale:", send<double>(window, "backingScaleFactor"));
    }
    state.queue->addResidencySet(layer->residencySet());

    Swapchain swapchain;
    swapchain.width = desc.width;
    swapchain.height = desc.height;
    swapchain.format = desc.format;
    swapchain.presentMode = desc.presentMode;
    swapchain.imageCount = static_cast<uint32>(layer->maximumDrawableCount());
    swapchain.internal.state = s;
    swapchain.internal.id = EQ_MTL_TRACK(&state);
    return swapchain;
}

void resizeSwapchain(Device &device, Swapchain &swapchain, uint32 width, uint32 height) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    DeviceState &state = *device.internal.state;
    EQ_MTL_CHECK_ALIVE(&state, swapchain.internal.id, "swapchain");
    if (width == 0 || height == 0) {
        reportError(ErrorCategory::eProgrammerError, "resizeSwapchain: the width and height must not be 0");
    }
    SwapchainState &s = *swapchain.internal.state;
    waitIdle(device);
    releaseTextures(state, s);
    s.layer->setDrawableSize(CGSize{static_cast<double>(width), static_cast<double>(height)});
    swapchain.width = width;
    swapchain.height = height;
}

void destroySwapchain(Device &device, Swapchain &swapchain) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    DeviceState &state = *device.internal.state;
    EQ_MTL_CHECK_ALIVE(&state, swapchain.internal.id, "swapchain");
    EQ_MTL_UNTRACK(&state, swapchain.internal.id);
    SwapchainState *s = swapchain.internal.state;
    waitIdle(device);
    releaseTextures(state, *s);
    state.queue->removeResidencySet(s->layer->residencySet());
    release(s->layer);
    delete s;
    swapchain = {};
}

Texture acquire(Device &device, Swapchain &swapchain) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    DeviceState &state = *device.internal.state;
    EQ_MTL_CHECK_ALIVE(&state, swapchain.internal.id, "swapchain");
    SwapchainState &s = *swapchain.internal.state;
    if (s.current != nullptr) {
        reportError(ErrorCategory::eProgrammerError, "acquire: the screen texture of this frame was not presented yet");
    }
    ScopedPool scope;
    // Blocks while every drawable is in use; nil after a second without one
    // (the layer's timeout), e.g. while the display sleeps: try again.
    CA::MetalDrawable *drawable = nullptr;
    while (drawable == nullptr) {
        drawable = s.layer->nextDrawable();
        checkGpuError(state);
    }
    s.current = drawable->retain();
    state.queue->wait(drawable);

    MTL::Texture *native = drawable->texture();
    auto found = std::ranges::find(s.textures, native, &std::pair<MTL::Texture *, Texture>::first);
    if (found == s.textures.end()) {
        // A layer keeps a few textures and makes new ones only when its size or
        // format changes: never more than a few entries between resizes.
        s.textures.emplace_back(native, wrap(state, s, native));
        found = std::prev(s.textures.end());
    }
    return found->second;
}

void present(Queue &queue, Swapchain &swapchain) {
    EQ_MTL_CHECK_THREAD(queue.internal.device);
    DeviceState &state = *queue.internal.device;
    EQ_MTL_CHECK_ALIVE(&state, swapchain.internal.id, "swapchain");
    SwapchainState &s = *swapchain.internal.state;
    if (s.current == nullptr) {
        reportError(ErrorCategory::eProgrammerError, "present: no screen texture was acquired");
    }
    // After the work committed before present, as P63 asks.
    state.queue->signalDrawable(s.current);
    s.current->present();
    release(s.current);
}

} // namespace RI::Metal
