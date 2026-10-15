// Swapchains on D3D12 (P43, P63, P64): a DXGI flip-model swapchain.
//
// Owns: surface formats and present modes, create / resize / destroy, acquire
// and present. acquire() takes the current back buffer once the GPU is done
// with its last frame; present() calls Present. Neither submits work: back
// buffers rest in COMMON, which is PRESENT, so RI's switches around rendering
// sections and clears return them ready to present.
//
// Watch out: sRGB formats use a UNORM buffer with sRGB render-target views,
// as flip model requires.
//
// Vulkan counterpart: backends/vulkan/Swapchain.cpp.

#include "D3D12Check.hpp"
#include "DeviceState.hpp"
#include "Formats.hpp"

#include <RI/Error.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <vector>

namespace RI::D3D12 {

struct SwapchainState {
    SwapchainDesc desc; // without its name, which `name` keeps
    std::string name;   // names the images, also those rebuilt on resize (P106)
    IDXGISwapChain3 *swapchain = nullptr;
    bool tearing = false; // created with ALLOW_TEARING
    std::vector<Texture> textures;
    std::vector<uint64> imageUse; // fence value after each buffer's last present
    uint32 current = UINT32_MAX;
};

namespace {

// Flip-model buffers have no sRGB formats: the buffer is UNORM, views are sRGB.
DXGI_FORMAT bufferFormat(Format format) {
    switch (format) {
    case Format::eBGRA8Srgb: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case Format::eRGBA8Srgb: return DXGI_FORMAT_R8G8B8A8_UNORM;
    default: return dxgiFormat(format);
    }
}

DXGI_COLOR_SPACE_TYPE colorSpaceOf(ColorSpace space) {
    switch (space) {
    case ColorSpace::eSrgb: return DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    case ColorSpace::eHdr10: return DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    case ColorSpace::eScRgb: return DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    }
    return DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
}

bool tearingSupported(DeviceState &state) {
    BOOL allowed = FALSE;
    IDXGIFactory5 *factory5 = nullptr;
    if (SUCCEEDED(state.factory->QueryInterface(IID_PPV_ARGS(&factory5)))) {
        if (FAILED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowed, sizeof(allowed)))) {
            allowed = FALSE;
        }
        release(factory5);
    }
    return allowed == TRUE;
}

// Whether an output of the adapter shows HDR10 (P64 (1)).
bool hdrOutput(DeviceState &state) {
    bool hdr = false;
    IDXGIOutput *output = nullptr;
    for (UINT i = 0; !hdr && state.adapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND; ++i) {
        IDXGIOutput6 *output6 = nullptr;
        if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&output6)))) {
            DXGI_OUTPUT_DESC1 desc{};
            hdr = SUCCEEDED(output6->GetDesc1(&desc)) && desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            release(output6);
        }
        release(output);
    }
    return hdr;
}

HWND hwndOf(const Window &window, const char *call) {
    if (window.system != WindowSystem::eWin32 || window.handle == nullptr) {
        reportError(ErrorCategory::eUnsupported, std::format("{}: D3D12 presents to Win32 windows (an HWND)", call));
    }
    return static_cast<HWND>(window.handle);
}

void waitFence(DeviceState &state, uint64 value) {
    if (value != 0 && state.fence->GetCompletedValue() < value) {
        checkQueueHr(state, state.fence->SetEventOnCompletion(value, nullptr), "ID3D12Fence::SetEventOnCompletion");
    }
}

void releaseBuffers(DeviceState &state, SwapchainState &s) {
    for (Texture &texture : s.textures) {
        EQ_DX_UNTRACK(&state, texture.internal.id);
        releaseTextureView(state, texture.view);
        auto *resource = static_cast<ID3D12Resource *>(texture.internal.resource);
        release(resource);
    }
    s.textures.clear();
    s.imageUse.clear();
}

void buildBuffers(DeviceState &state, Swapchain &swapchain) {
    SwapchainState &s = *swapchain.internal.state;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    checkHr(s.swapchain->GetDesc1(&desc), "IDXGISwapChain1::GetDesc1");
    const TextureUsage usage = TextureUsage::eRenderTarget | TextureUsage::eCopyDst;
    for (UINT i = 0; i < desc.BufferCount; ++i) {
        ID3D12Resource *buffer = nullptr;
        checkHr(s.swapchain->GetBuffer(i, IID_PPV_ARGS(&buffer)), "IDXGISwapChain::GetBuffer");
        Texture texture;
        texture.width = desc.Width;
        texture.height = desc.Height;
        texture.format = s.desc.format.format;
        texture.usage = usage;
        texture.internal.resource = buffer;
        texture.internal.restingLayout = static_cast<uint32>(D3D12_BARRIER_LAYOUT_COMMON); // = PRESENT; allows copies
        texture.internal.viewFormats = uint64{1} << static_cast<uint32>(texture.format);
        texture.internal.id = EQ_DX_TRACK(&state);
        texture.view = makeTextureView(state, texture, {});
        if (!s.name.empty()) {
            applyName(state, texture, std::format("{} [{}]", s.name, i));
        }
        s.textures.push_back(texture);
    }
    s.imageUse.assign(desc.BufferCount, 0);
    s.current = UINT32_MAX;
    swapchain.width = desc.Width;
    swapchain.height = desc.Height;
    swapchain.imageCount = desc.BufferCount;
}

} // namespace

std::vector<SurfaceFormat> surfaceFormats(Device &device, const Window &window) {
    EQ_DX_CHECK_THREAD(device.internal.state);
    hwndOf(window, "surfaceFormats");
    std::vector<SurfaceFormat> formats = {
        {Format::eBGRA8Srgb, ColorSpace::eSrgb},     {Format::eBGRA8Unorm, ColorSpace::eSrgb},
        {Format::eRGBA8Srgb, ColorSpace::eSrgb},     {Format::eRGBA8Unorm, ColorSpace::eSrgb},
        {Format::eRGB10A2Unorm, ColorSpace::eSrgb},  {Format::eRGBA16Float, ColorSpace::eScRgb},
    };
    if (hdrOutput(*device.internal.state)) {
        formats.push_back({Format::eRGB10A2Unorm, ColorSpace::eHdr10});
    }
    return formats;
}

bool presentModeSupported(Device &device, const Window &window, PresentMode mode) {
    EQ_DX_CHECK_THREAD(device.internal.state);
    hwndOf(window, "presentModeSupported");
    return mode != PresentMode::eImmediate || tearingSupported(*device.internal.state);
}

// Flip-model back buffers never allow unordered access (P64 (5)).
bool screenStorageSupported([[maybe_unused]] Device &device, const Window &, Format) {
    EQ_DX_CHECK_THREAD(device.internal.state);
    return false;
}

Swapchain createSwapchain(Device &device, const SwapchainDesc &desc) {
    EQ_DX_CHECK_THREAD(device.internal.state);
    DeviceState &state = *device.internal.state;
    const HWND hwnd = hwndOf(desc.window, "createSwapchain");
    if (desc.storage) {
        reportError(ErrorCategory::eUnsupported,
                    "createSwapchain: D3D12 screen textures cannot be written by compute (P64 (5)); render into a "
                    "texture and copy it to the screen");
    }
    const std::vector<SurfaceFormat> formats = surfaceFormats(device, desc.window);
    if (std::ranges::find(formats, desc.format) == formats.end() || !presentModeSupported(device, desc.window, desc.presentMode)) {
        reportError(ErrorCategory::eUnsupported,
                    "createSwapchain: this window does not support the format, color space or present mode; ask "
                    "surfaceFormats and presentModeSupported first (P64)");
    }
    auto *s = new SwapchainState;
    s->desc = desc;
    s->name = desc.name;
    s->desc.name = {};
    s->tearing = tearingSupported(state);
    const DXGI_SWAP_CHAIN_DESC1 chainDesc{
        .Width = desc.width,
        .Height = desc.height,
        .Format = bufferFormat(desc.format.format),
        .SampleDesc = {.Count = 1, .Quality = 0},
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount = desc.imageCount != 0 ? std::clamp(desc.imageCount, 2u, 16u) : 3u,
        .Scaling = DXGI_SCALING_STRETCH,
        .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
        .AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED,
        .Flags = s->tearing ? static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) : 0u,
    };
    IDXGISwapChain1 *swapchain1 = nullptr;
    checkHr(state.factory->CreateSwapChainForHwnd(state.graphicsQueue, hwnd, &chainDesc, nullptr, nullptr, &swapchain1),
            "CreateSwapChainForHwnd");
    checkHr(swapchain1->QueryInterface(IID_PPV_ARGS(&s->swapchain)), "IDXGISwapChain3");
    release(swapchain1);
    state.factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    checkHr(s->swapchain->SetColorSpace1(colorSpaceOf(desc.format.colorSpace)), "IDXGISwapChain3::SetColorSpace1");

    Swapchain swapchain;
    swapchain.format = desc.format;
    swapchain.presentMode = desc.presentMode;
    swapchain.internal.state = s;
    swapchain.internal.id = EQ_DX_TRACK(&state);
    buildBuffers(state, swapchain);
    return swapchain;
}

void resizeSwapchain(Device &device, Swapchain &swapchain, uint32 width, uint32 height) {
    EQ_DX_CHECK_THREAD(device.internal.state);
    DeviceState &state = *device.internal.state;
    EQ_DX_CHECK_ALIVE(&state, swapchain.internal.id, "swapchain");
    SwapchainState &s = *swapchain.internal.state;
    waitFence(state, state.lastSignaled);
    releaseBuffers(state, s);
    checkHr(s.swapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN,
                                       s.tearing ? static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) : 0u),
            "IDXGISwapChain::ResizeBuffers");
    buildBuffers(state, swapchain);
}

void destroySwapchain(Device &device, Swapchain &swapchain) {
    EQ_DX_CHECK_THREAD(device.internal.state);
    DeviceState &state = *device.internal.state;
    EQ_DX_CHECK_ALIVE(&state, swapchain.internal.id, "swapchain");
    EQ_DX_UNTRACK(&state, swapchain.internal.id);
    SwapchainState *s = swapchain.internal.state;
    waitFence(state, state.lastSignaled);
    releaseBuffers(state, *s);
    release(s->swapchain);
    delete s;
    swapchain = {};
}

Texture acquire(Device &device, Swapchain &swapchain) {
    EQ_DX_CHECK_THREAD(device.internal.state);
    DeviceState &state = *device.internal.state;
    EQ_DX_CHECK_ALIVE(&state, swapchain.internal.id, "swapchain");
    SwapchainState &s = *swapchain.internal.state;
    if (s.current != UINT32_MAX) {
        reportError(ErrorCategory::eProgrammerError, "acquire: the screen texture of this frame was not presented yet");
    }
    const UINT index = s.swapchain->GetCurrentBackBufferIndex();
    waitFence(state, s.imageUse[index]); // blocks while the buffer is still in use
    s.current = index;
    return s.textures[index];
}

void present(Queue &queue, Swapchain &swapchain) {
    EQ_DX_CHECK_THREAD(queue.internal.device);
    DeviceState &state = *queue.internal.device;
    EQ_DX_CHECK_ALIVE(&state, swapchain.internal.id, "swapchain");
    SwapchainState &s = *swapchain.internal.state;
    if (s.current == UINT32_MAX) {
        reportError(ErrorCategory::eProgrammerError, "present: no screen texture was acquired");
    }
    const uint32 index = s.current;
    s.current = UINT32_MAX;
    // eFifo: wait for vertical blank. eMailbox: flip model without waiting
    // drops queued frames. eImmediate: tearing.
    const UINT interval = s.desc.presentMode == PresentMode::eFifo ? 1 : 0;
    const UINT flags = s.desc.presentMode == PresentMode::eImmediate && s.tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
    checkQueueHr(state, s.swapchain->Present(interval, flags), "IDXGISwapChain::Present");
    const uint64 value = state.lastSignaled + 1;
    checkQueueHr(state, state.graphicsQueue->Signal(state.fence, value), "ID3D12CommandQueue::Signal");
    state.lastSignaled = value;
    s.imageUse[index] = value;
}

} // namespace RI::D3D12
