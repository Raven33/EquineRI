// Swapchains (P43, P63, P64): a DXGI flip-model swapchain. acquire() takes the
// current back buffer once the GPU is done with its last frame and runs a
// prerecorded switch from "undefined" (contents dropped) to its resting
// layout; present() runs the switch to PRESENT, then Present. sRGB formats
// use a UNORM buffer with sRGB render-target views, as flip model requires.

#include "Barriers.hpp"
#include "D3D12Check.hpp"
#include "DeviceState.hpp"
#include "Formats.hpp"

#include <RI/Error.hpp>

#include <algorithm>
#include <format>
#include <vector>

namespace RI::D3D12 {

struct SwapchainState {
    SwapchainDesc desc;
    IDXGISwapChain3 *swapchain = nullptr;
    bool tearing = false; // created with ALLOW_TEARING
    ID3D12CommandAllocator *allocator = nullptr;
    std::vector<Texture> textures;
    std::vector<ID3D12GraphicsCommandList7 *> toRest;
    std::vector<ID3D12GraphicsCommandList7 *> toPresent;
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

ID3D12GraphicsCommandList7 *recordSwitch(DeviceState &state, ID3D12CommandAllocator *allocator, const TextureView &view,
                                         const TextureBarrier &barrier) {
    ID3D12GraphicsCommandList7 *list = nullptr;
    checkHr(state.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list)),
            "CreateCommandList (swapchain)");
    textureBarrier(list, view, barrier);
    checkHr(list->Close(), "ID3D12GraphicsCommandList::Close");
    return list;
}

void releaseBuffers(DeviceState &state, SwapchainState &s) {
    for (Texture &texture : s.textures) {
        EQ_DX_UNTRACK(&state, texture.internal.id);
        releaseTextureView(state, texture.view);
        auto *resource = static_cast<ID3D12Resource *>(texture.internal.resource);
        release(resource);
    }
    for (ID3D12GraphicsCommandList7 *&list : s.toRest) {
        release(list);
    }
    for (ID3D12GraphicsCommandList7 *&list : s.toPresent) {
        release(list);
    }
    s.textures.clear();
    s.toRest.clear();
    s.toPresent.clear();
    s.imageUse.clear();
    release(s.allocator);
}

void buildBuffers(DeviceState &state, Swapchain &swapchain) {
    SwapchainState &s = *swapchain.internal.state;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    checkHr(s.swapchain->GetDesc1(&desc), "IDXGISwapChain1::GetDesc1");
    checkHr(state.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.allocator)),
            "CreateCommandAllocator (swapchain)");
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
        texture.internal.restingLayout = static_cast<uint32>(restingLayout(usage));
        texture.internal.viewFormats = uint64{1} << static_cast<uint32>(texture.format);
        texture.internal.id = EQ_DX_TRACK(&state);
        texture.view = makeTextureView(state, texture, {});
        const auto resting = static_cast<D3D12_BARRIER_LAYOUT>(texture.internal.restingLayout);
        s.toRest.push_back(recordSwitch(state, s.allocator, texture.view,
                                        {.before = D3D12_BARRIER_LAYOUT_UNDEFINED,
                                         .after = resting,
                                         .syncBefore = D3D12_BARRIER_SYNC_NONE,
                                         .syncAfter = D3D12_BARRIER_SYNC_ALL,
                                         .accessBefore = D3D12_BARRIER_ACCESS_NO_ACCESS,
                                         .accessAfter = D3D12_BARRIER_ACCESS_COMMON,
                                         .discard = true}));
        s.toPresent.push_back(recordSwitch(state, s.allocator, texture.view,
                                           {.before = resting,
                                            .after = D3D12_BARRIER_LAYOUT_PRESENT,
                                            .syncBefore = D3D12_BARRIER_SYNC_ALL,
                                            .syncAfter = D3D12_BARRIER_SYNC_NONE,
                                            .accessBefore = D3D12_BARRIER_ACCESS_COMMON,
                                            .accessAfter = D3D12_BARRIER_ACCESS_NO_ACCESS}));
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
    hwndOf(window, "presentModeSupported");
    return mode != PresentMode::eImmediate || tearingSupported(*device.internal.state);
}

// Flip-model back buffers never allow unordered access (P64 (5)).
bool screenStorageSupported(Device &, const Window &, Format) { return false; }

Swapchain createSwapchain(Device &device, const SwapchainDesc &desc) {
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
    DeviceState &state = *device.internal.state;
    EQ_DX_CHECK_ALIVE(&state, swapchain.internal.id, "swapchain");
    SwapchainState &s = *swapchain.internal.state;
    if (s.current != UINT32_MAX) {
        reportError(ErrorCategory::eProgrammerError, "acquire: the screen texture of this frame was not presented yet");
    }
    const UINT index = s.swapchain->GetCurrentBackBufferIndex();
    waitFence(state, s.imageUse[index]); // blocks while the buffer is still in use
    ID3D12CommandList *lists[] = {s.toRest[index]};
    state.graphicsQueue->ExecuteCommandLists(1, lists);
    s.current = index;
    return s.textures[index];
}

void present(Queue &queue, Swapchain &swapchain) {
    DeviceState &state = *queue.internal.device;
    EQ_DX_CHECK_ALIVE(&state, swapchain.internal.id, "swapchain");
    SwapchainState &s = *swapchain.internal.state;
    if (s.current == UINT32_MAX) {
        reportError(ErrorCategory::eProgrammerError, "present: no screen texture was acquired");
    }
    const uint32 index = s.current;
    s.current = UINT32_MAX;
    ID3D12CommandList *lists[] = {s.toPresent[index]};
    state.graphicsQueue->ExecuteCommandLists(1, lists);
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
