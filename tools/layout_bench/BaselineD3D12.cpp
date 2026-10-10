// The D3D12 baseline (P79): hand-written optimal per-texture enhanced barriers.
// Each texture is in the layout of its current role, with exact sync and
// access on both sides of every barrier.

#include "Bench.hpp"

#include <RI/D3D12/Native.hpp>

#include <algorithm>
#include <vector>

namespace bench {

namespace {

struct State {
    D3D12_BARRIER_LAYOUT layout = D3D12_BARRIER_LAYOUT_UNDEFINED;
    D3D12_BARRIER_SYNC sync = D3D12_BARRIER_SYNC_NONE;
    D3D12_BARRIER_ACCESS access = D3D12_BARRIER_ACCESS_NO_ACCESS;
};

struct Target {
    D3D12_BARRIER_LAYOUT layout;
    D3D12_BARRIER_SYNC sync;
    D3D12_BARRIER_ACCESS access;
    bool write;
};

Target targetOf(Role role, PassKind kind) {
    switch (role) {
    case Role::eColorTarget:
        return {D3D12_BARRIER_LAYOUT_RENDER_TARGET, D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET,
                true};
    case Role::eDepthTarget:
        return {D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE, D3D12_BARRIER_SYNC_DEPTH_STENCIL,
                D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE, true};
    case Role::eSampled:
        return {D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
                kind == PassKind::eCompute ? D3D12_BARRIER_SYNC_COMPUTE_SHADING : D3D12_BARRIER_SYNC_PIXEL_SHADING,
                D3D12_BARRIER_ACCESS_SHADER_RESOURCE, false};
    case Role::eStorage:
        return {D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS, D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, true};
    }
    return {};
}

} // namespace

struct Baseline {
    RI::Device *device = nullptr;
    RI::DescriptorHeap *heap = nullptr;
    std::vector<State> states;
};

Baseline *createBaseline(RI::Device &device, RI::DescriptorHeap &heap) { return new Baseline{&device, &heap, {}}; }
void destroyBaseline(Baseline *baseline) { delete baseline; }

// D3D12 descriptors carry no layout: RI's writes serve the baseline too.
void baselineWriteDescriptors(Baseline &b, Workload &w) {
    for (Texture &t : w.textures) {
        if ((t.texture.usage & RI::TextureUsage::eSampled) != RI::TextureUsage::eNone) {
            RI::writeDescriptors(*b.device, *b.heap, {{.slot = t.sampledSlot, .descriptor = RI::ReadView{.texture = t.sampledView}}});
        }
        if ((t.texture.usage & RI::TextureUsage::eStorage) != RI::TextureUsage::eNone) {
            RI::writeDescriptors(*b.device, *b.heap,
                                 {{.slot = t.storageSlot, .descriptor = RI::ReadWriteView{.texture = t.texture.view}}});
        }
    }
}

void baselineReset(Baseline &b, Workload &w) {
    b.states.assign(w.textures.size(), {});
    for (size_t i = 0; i < w.textures.size(); ++i) {
        b.states[i].layout = RI::D3D12::native(w.textures[i].texture).restingLayout;
        b.states[i].sync = D3D12_BARRIER_SYNC_NONE;
        b.states[i].access = D3D12_BARRIER_ACCESS_NO_ACCESS;
    }
}

void baselineBeforePass(Baseline &b, RI::CommandList &cmd, Workload &w, const Pass &pass) {
    std::vector<D3D12_TEXTURE_BARRIER> barriers;
    for (const Use &use : pass.uses) {
        State &s = b.states[use.texture];
        const Target t = targetOf(use.role, pass.kind);
        const bool previousWrite = s.access != D3D12_BARRIER_ACCESS_NO_ACCESS &&
                                   (s.access & (D3D12_BARRIER_ACCESS_RENDER_TARGET | D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE |
                                                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS)) != 0;
        if (s.layout == t.layout && !previousWrite && !t.write) {
            s.sync |= t.sync; // read after read: no barrier
            continue;
        }
        const RI::Texture &tex = w.textures[use.texture].texture;
        const bool depth = RI::isDepthFormat(tex.format);
        const bool undefined = use.discard;
        D3D12_TEXTURE_BARRIER barrier{
            .SyncBefore = s.sync,
            .SyncAfter = t.sync,
            .AccessBefore = undefined || s.sync == D3D12_BARRIER_SYNC_NONE ? D3D12_BARRIER_ACCESS_NO_ACCESS : s.access,
            .AccessAfter = t.access,
            .LayoutBefore = undefined ? D3D12_BARRIER_LAYOUT_UNDEFINED : s.layout,
            .LayoutAfter = t.layout,
            .pResource = RI::D3D12::native(tex).resource,
            .Subresources = {.IndexOrFirstMipLevel = 0xffffffff},
            .Flags = undefined && (use.role == Role::eColorTarget || use.role == Role::eDepthTarget)
                         ? D3D12_TEXTURE_BARRIER_FLAG_DISCARD
                         : D3D12_TEXTURE_BARRIER_FLAG_NONE,
        };
        (void)depth;
        barriers.push_back(barrier);
        s = {t.layout, t.sync, t.access};
    }
    if (!barriers.empty()) {
        const D3D12_BARRIER_GROUP group{.Type = D3D12_BARRIER_TYPE_TEXTURE,
                                        .NumBarriers = static_cast<UINT32>(barriers.size()),
                                        .pTextureBarriers = barriers.data()};
        RI::D3D12::native(cmd)->Barrier(1, &group);
    }
}

void baselineBeginRendering(Baseline &, RI::CommandList &cmd, Workload &w, const Pass &pass) {
    ID3D12GraphicsCommandList7 *list = RI::D3D12::native(cmd);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[8] = {};
    UINT count = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    bool hasDepth = false;
    uint32 width = 0, height = 0;
    for (const Use &use : pass.uses) {
        const RI::TextureView &view = w.textures[use.texture].texture.view;
        if (use.role == Role::eColorTarget) {
            rtvs[count] = {static_cast<SIZE_T>(view.internal.rtv)};
            if (use.discard) {
                const float clear[4] = {0, 0, 0, 0};
                list->ClearRenderTargetView(rtvs[count], clear, 0, nullptr);
            }
            ++count;
        } else if (use.role == Role::eDepthTarget) {
            dsv = {static_cast<SIZE_T>(view.internal.dsv)};
            hasDepth = true;
            if (use.discard) {
                D3D12_CLEAR_FLAGS flags = D3D12_CLEAR_FLAG_DEPTH;
                if (RI::hasStencil(view.format)) {
                    flags |= D3D12_CLEAR_FLAG_STENCIL;
                }
                list->ClearDepthStencilView(dsv, flags, 1.0f, 0, 0, nullptr);
            }
        } else {
            continue;
        }
        width = view.width;
        height = view.height;
    }
    list->OMSetRenderTargets(count, count > 0 ? rtvs : nullptr, FALSE, hasDepth ? &dsv : nullptr);
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
}

void baselineEndRendering(Baseline &, RI::CommandList &) {}

} // namespace bench
