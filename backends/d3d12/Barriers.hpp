#pragma once

// RI's own texture barriers on D3D12 (A', P68, P69): layout switches around
// RI's operations, with an exact half on RI's side and a full half on the
// other.

#include <RI/D3D12/Backend.hpp>

#include <d3d12.h>

namespace RI::D3D12 {

struct TextureBarrier {
    D3D12_BARRIER_LAYOUT before = D3D12_BARRIER_LAYOUT_UNDEFINED;
    D3D12_BARRIER_LAYOUT after = D3D12_BARRIER_LAYOUT_UNDEFINED;
    D3D12_BARRIER_SYNC syncBefore = D3D12_BARRIER_SYNC_ALL;
    D3D12_BARRIER_SYNC syncAfter = D3D12_BARRIER_SYNC_ALL;
    D3D12_BARRIER_ACCESS accessBefore = D3D12_BARRIER_ACCESS_COMMON;
    D3D12_BARRIER_ACCESS accessAfter = D3D12_BARRIER_ACCESS_COMMON;
    bool discard = false;
};

// One barrier over the view's mips, layers and planes.
void textureBarrier(ID3D12GraphicsCommandList7 *list, const TextureView &view, const TextureBarrier &barrier);

// A texture's resting layout (L1): DIRECT_QUEUE_COMMON (SRV, UAV and copy on the
// direct queue) when it is read or written by shaders or copies; render-target-
// or depth-only textures rest in their attachment layout.
D3D12_BARRIER_LAYOUT restingLayout(TextureUsage usage);

} // namespace RI::D3D12
