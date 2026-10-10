# EquineRI API style (2026-10-10)

Status: **style decided** (P80–P86 in CONTEXT.md). The sketches below show the decided
style; exact names and signatures are still illustrative. Replaces U1 (the hello-world),
which the user dropped.

## Layering (P80)

EquineRI is designed for itself. The layer built on top of it is not RI's concern and does
not shape RI's API.

## Decided style

| P | Rule |
|---|---|
| P81 | Thin value objects: copyable structs holding native handles and cached facts; no ownership, no lookup, no RAII. |
| P82 | Plain data structs with public fields; facts are fields (`buf.size`, `buf.gpuAddress`); operations are free functions (`RI::createBuffer(device, …)`); hidden state in an unnamed `internal` struct field; no `private`/`protected`; methods only when clearly justified. |
| P83 | One namespace per backend (`RI::Vk::`, `RI::D3D12::`), nothing shared; `RI::` is an alias chosen by the build; consistency via a conformance file compiled per backend. |
| P84 | Native access is gated: include `<RI/Vk/Native.hpp>` and name the backend (`RI::Vk::native(device)`); `#if EQUINE_BACKEND_*` blocks for per-backend native code. |
| P85 | Recording functions carry a `cmd` prefix (`RI::cmdDispatch(cmd, …)`), as Vulkan's `vkCmd*`. |
| P86 | Descriptions are plain structs filled with C++20 designated initializers and defaults (no builders, no long argument lists). Naming: `RI::` namespace, `camelCase` functions, `PascalCase` types, `e`-prefixed enumerators, flags combined with `\|`. |

Rejected along the way: handles with a lookup table (A) and RAII objects (C) for P81;
methods on the device; a hidden global device; one shared device struct with `.vk` /
`.d3d12` sub-structs; ungated native fields (`device.device`); a `RI::Cmd::` namespace and
methods on the command list for P85.

## Sketch 1: buffer → compute → readback

```cpp
RI::Device device = RI::createDevice({.gpu = RI::listGpus()[0]});              // P60
RI::Queue  queue  = device.graphicsQueue;                                      // P37

// Memory and resources (P31, P32, P58)
RI::Memory gpuMem  = RI::allocateMemory(device, RI::MemoryKind::eGPU,      1 << 20);
RI::Memory readMem = RI::allocateMemory(device, RI::MemoryKind::eReadback, 1 << 20);
RI::Buffer data    = RI::createBuffer(device, gpuMem, 0, {
    .size  = 4096,
    .usage = RI::BufferUsage::eStorage | RI::BufferUsage::eCopySrc,
});
RI::Buffer result  = RI::createBuffer(device, readMem, 0, {.size = 4096, .usage = RI::BufferUsage::eCopyDst});

// Descriptors (P42, P59)
RI::DescriptorHeap heap = RI::createDescriptorHeap(device, {.resourceCount = 1024, .samplerCount = 16});
RI::writeDescriptors(device, heap, {{.slot = 0, .descriptor = RI::ReadWriteView{.buffer = data}}});   // P87; batching form open (D1)

// Pipeline (P38, P45): bytecode in; root data marked with EQ_ROOT_DATA in the shader
RI::Pipeline fill = RI::createComputePipeline(device, {.shader = loadBytecode("fill.cs")});

// Record (P34, P85) and submit (P20)
RI::CommandList cmd = RI::createCommandList(device);
RI::cmdBegin(cmd);
RI::cmdSetHeap(cmd, heap);
RI::cmdSetPipeline(cmd, fill);
RI::cmdSetRootData(cmd, FillRoot{.outputSlot = 0, .value = 42.0f});
RI::cmdDispatch(cmd, 4096 / 4 / 64, 1, 1);
RI::cmdBarrier(cmd, RI::Stage::eCompute, RI::Stage::eCopy);    // P36: coarse stage → stage
RI::cmdCopyBuffer(cmd, data, 0, result, 0, 4096);
RI::cmdEnd(cmd);

RI::TimelinePoint done = RI::submit(queue, cmd);
RI::wait(queue, done);
const float* values = static_cast<const float*>(result.mapped);   // Readback memory stays mapped
```

## Sketch 2: draw to the screen

```cpp
RI::Swapchain swapchain = RI::createSwapchain(device, {
    .window  = window,                               // P43: a native window handle; RI creates no window
    .format  = RI::Format::eB8G8R8A8Unorm,           // P64 (a helper picks defaults)
    .present = RI::PresentMode::eFifo,
});

RI::Pipeline tri = RI::createGraphicsPipeline(device, {
    .mesh         = loadBytecode("tri.ms"),          // vertex pulling / mesh shaders (P39)
    .pixel        = loadBytecode("tri.ps"),
    .colorFormats = {swapchain.format},
});

while (running) {
    RI::TextureView screen = RI::currentTexture(swapchain);   // P63: D3D12 model, no copy
    RI::resetCommandList(cmd);                                 // P56: after its timeline point
    RI::cmdBegin(cmd);
    RI::cmdBeginRendering(cmd, {
        .color = {{.view = screen, .load = RI::Load::eClear, .clear = {0, 0, 0, 1}}},
    });
    RI::cmdSetPipeline(cmd, tri);
    RI::cmdDrawMeshTasks(cmd, 1, 1, 1);
    RI::cmdEndRendering(cmd);
    RI::cmdEnd(cmd);
    lastFrame = RI::submit(queue, cmd);
    RI::present(swapchain);
    RI::wait(queue, lastFrame);   // simplest form; overlapping frames is a helper (P20)
}
```

## Sketch 3: a native section (P27, P84)

```cpp
#if EQUINE_BACKEND_VULKAN
#include <RI/Vk/Native.hpp>
#endif

RI::cmdDispatch(cmd, 8, 8, 1);
#if EQUINE_BACKEND_VULKAN
RI::Vk::NativeSection n = RI::Vk::cmdBeginNative(cmd);    // native command buffer + resting layouts
vkCmdSomethingNew(n.commandBuffer, ...);
RI::Vk::cmdEndNative(cmd, {});                            // out map (P27); state restored (P70)
#else
#error "SomethingNew is not implemented on this backend"
#endif
RI::cmdDispatch(cmd, 8, 8, 1);
```

## Decided: descriptor kind names (P87, proposal A)

User (2026-10-10): "View" in the name clarifies; `ConstView`-like names obscure what a
CBV is; free helpers `srv()/uav()/cbv()` are fine, but the plain names must be good for
people who don't use them.

| Shader use | D3D12 | Vulkan | Proposal A (recommended) | Proposal B (short) |
|---|---|---|---|---|
| read texture / buffer / TLAS | SRV | sampled image / read-only storage buffer / acceleration structure | `ReadView` | `RView` |
| read-write texture / buffer | UAV | storage image / storage buffer | `ReadWriteView` | `RWView` |
| constant buffer | CBV | uniform buffer | `ConstantBufferView` (D3D12's own long name) | `CBView` |
| sampler | sampler | sampler | `RI::Sampler` itself | same |

Fields name the target: `.texture` (a P73 texture view), `.buffer` (+ range, stride/format
for typed buffers), `.accelerationStructure`. Optional helpers: `RI::srv(x)`, `RI::uav(x)`,
`RI::cbv(x)`.

## Decided: where RI's own checks live (P88, proposals 2 + 3)

User: no preference on the mechanism; asked for proposals.

1. **Forwarding validation backend** (`RI::Validation::`, the `RI::` alias points at it in
   validation builds; it checks, then calls the real backend). Checks written once; real
   backends untouched; but wrapper structs, native access must unwrap, conformance must
   cover it.
2. **Shared check functions called by each backend.** A common `validation/` folder of
   free functions (`validateTextureDesc(desc)`, `validateBarrier(...)`, ...). Each backend
   function starts with `EQ_VALIDATE(check(...));`, which compiles to nothing in release.
   Tracking state (destroyed objects, memory placements for P69, in-flight slots for P59,
   query resolves for P71) lives in `internal` under `#if EQUINE_VALIDATION`. Checks
   written once, no wrappers, no unwrapping; one line per backend function.
3. **Scope reduction (combines with 1 or 2).** Leave everything native layers and GPU-AV
   already catch to them (P53); RI checks only what they cannot see: RI's own rules
   (discard after aliasing P69, native-section out map P27, root-data marking and size
   P45, read-only depth P75, view formats P74, slot reuse P59, timeline misuse P56).

Recommendation: **2 + 3**.
