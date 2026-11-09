# Proposal: a Metal backend (2026-11-07)

A third backend, `RI::Metal::`, on Metal 4 and Apple silicon, from the answers recorded in
`docs/MetalBackend-Handoff-2026-11-07.md` §4. Nothing is implemented. **Approved by the user
(2026-11-08)** with the answers at the end; recorded as P113–P116 in `CONTEXT.md`.

**Evidence.** Facts marked **checked** were confirmed on the dev Mac (Apple M4, macOS 27,
Xcode 27.2): against the SDK headers, metal-cpp, the MSC headers and manual, MSC runs on RI's test
shaders, or a probe program (`IRDescriptorTableEntry` heaps, root data and samplers on a Metal 4
compute encoder). Everything else is read from headers but not run, and is marked **to verify**.
Each such point is checked when its milestone is built.

## 1. Toolchain and pins

| Piece | Pin | How |
|---|---|---|
| Metal API | Metal 4 (`MTL4*`, macOS 26+), Apple silicon (M1+, Apple7+) | Metal 3 path later (§12); nothing below takes away that option |
| metal-cpp | `release/metal-cpp_macOS27.0.1_iOS27.0.1` (commit `8cdde68`) | Git submodule `third_party/metal-cpp`, as Vulkan-Headers; it has all 30 `MTL4*` headers (**checked**) |
| Metal Shader Converter | 4.0 beta 3 (`metal-irconverter` 4.0.3), pkg SHA-256 `4498332…6be622` | Not redistributable in the repo; the user's copy, laid out like the DXC build (`EQUINE_MSC_DIR` → `bin/`, `include/`, `lib/`), read by `cmake/EquineMSC.cmake` |
| Metal compiler | Xcode's Metal Toolchain | Only for RI's own MSL kernels, if §7 needs them |

The MSC CLI is self-contained (no dylib dependency, **checked**). The dylib is 60 MB (x86_64 and
arm64). MSC 4.0 beta 3 crashes (SIGSEGV) on a root signature JSON whose `Flags` is the string
`"A | B"`. A number (`3072`) works (**checked**), so RI writes the flags as a number.

## 2. Shaders

### 2.1 Conversion of RI's shaders

RI's D3D12 root signature is one fixed shape: 32-bit root constants at `b0, space999` (P45) and
the two heap-indexing flags (P94). MSC therefore gets **one fixed root signature for every
shader**: 64 constants (`kMaxRootDataSize` = 256 bytes) and flags `0xC00`. Every converted shader
reports the same top-level argument buffer: 256 bytes of constants at offset 0, nothing else
(**checked**, reflection JSON). A shader reads only the constants it declares. So the build needs
no per-shader root data size, and the Metal backend binds root data the same way for every
pipeline.

Every DXIL that RI builds or tests was converted with `--minimum-gpu-family=Apple7
--minimum-os-build-version=26.0.0` (**checked**):

| Converts | Shaders |
|---|---|
| Yes | compute (`fill`, `root4`, `root64`, `root_arrays`, `write_faces`), vertex, pixel (`triangle_vs`, `solid_ps`, `link_*`, `storage_raster_ps`), mesh and amplification (`fullscreen_ms`, `fullscreen_payload_ms`, `fullscreen_as`), ray query (`ray_query`), hull, domain and geometry (`patch_*`, `fullscreen_gs`, `layered_gs`; see §9) |
| No, as intended | `declared`, `sampler_heap`, `misuse_b1`, `misuse_extra`: declared resources, which RI refuses anyway (P30). These are DXIL-reader test inputs only |
| No | `atomic64`: `dx.op.atomicBinOp.i64` on a heap buffer (§9) |

### 2.2 What RI consumes on Metal (decision)

A metallib alone is not enough. Metal 4 takes the threadgroup size at dispatch
(`dispatchThreadgroups(groups, threadsPerThreadgroup)`), and RI also needs root data size, mesh
output counts, payload size, stage signatures for P101 and the int64 flags (P105 (3)). RI's DXIL
reader (P67) already reads all of these from the DXIL. Three options:

- **(a) DXIL container plus a metallib part (recommended).** `equine_add_shaders` converts each
  DXIL and writes `<name>.mtl.dxil`: the DXIL container with one more part, `MTLB`, holding the
  metallib. RI's Metal backend reads its facts with the existing DXIL reader (PSV0, SFI0, ISG1,
  OSG1) and creates the library from `MTLB`. One file, no new reflection code, the same checks as
  D3D12. The file is not valid for D3D12 (the hash no longer matches), and only Metal reads it.
- (b) A metallib plus MSC's reflection JSON, which RI parses. This adds a JSON reader to RI, and
  the JSON is MSC's format, not RI's.
- (c) RI takes plain DXIL and converts at pipeline creation through `libmetalirconverter`. One
  bytecode for D3D12 and Metal, but a 60 MB closed-source runtime dependency, against P67's rule
  of no compiler at run time.

The bytecode field stays `std::span<const std::byte>`, so no signature changes.

### 2.3 Build integration

`equine_add_shaders` gains a third output on Apple hosts: DXIL → `metal-shaderconverter` →
`.mtl.dxil`. It uses the fixed root signature (written by CMake), `--minimum-gpu-family=Apple7`
and `--minimum-os-build-version=26.0.0`. Shaders that cannot convert (the misuse inputs) are
marked `NO_METAL` in their `equine_add_shaders` call, so the build stays explicit. On macOS, DXIL
stays unsigned (no libdxil), which matters only for D3D12.

## 3. Binding model

### 3.1 Descriptor heaps (P94, P99)

- **Resource heap:** an `MTLBuffer` of `resourceCount` × 24-byte `IRDescriptorTableEntry`
  (`gpuVA`, `textureViewID`, `metadata`), bound at argument-table buffer index 0
  (`kIRDescriptorHeapBindPoint`). **Sampler heap:** the same at index 1. **Checked:** sizes, bind
  points, and a probe reading through both heaps.
- `writeDescriptors` writes entries directly into the buffer from the CPU
  (`IRDescriptorTableSetBuffer` or `SetTexture`, or RI's own three stores). 10^6 buffer entries
  take 3.5 ms on the M4 (**checked**). The heap is shared storage and lives as long as the
  `DescriptorHeap`. Like D3D12's shader-visible heap, it is RI's own fixed object.
- **Buffer views:** `gpuVA` = address + offset, low 32 bits of `metadata` = length. The probe's
  `GetDimensions` on structured and raw views, and constant buffers, read correctly
  (**checked**). Typed views carry the 16-byte texture-buffer alignment offset in bits 32–47 (to
  verify). Out-of-bounds reads are not clamped: the probe read data past the view. MSC's
  `--boundsCheck` adds clamping at a cost (question Q6).
- **Texture views:** an `MTLTexture` view (`newTextureView`) per RI view, whose `gpuResourceID`
  goes in `textureViewID`. A view starting at mip 1 sampled correctly (**checked**). RI owns the
  view object, as Vulkan owns its image views.

### 3.2 Samplers (P99)

Metal needs a sampler object (`MTLSamplerState`, `supportArgumentBuffers = true`), and the heap
entry holds its `gpuResourceID`. Metal returns the same ID for an identical description
(**checked**). So `writeSamplers` keeps its form: RI creates or reuses a sampler object per
`SamplerDesc` in a device cache, as the Vulkan backend's sampler cache does (P99). LOD bias goes
in the sampler object, which needs no extra MSC flag (**checked**, probe (c)). Conformance tests
pin down which sample instructions apply it.

### 3.3 Root data (P44, P45, P107 (2))

Root data is the top-level argument buffer (index 2, `kIRArgumentBufferBindPoint`): the bytes at
a GPU address in the argument table. **Metal 4 has no `setBytes`.** An argument table holds only
addresses and resource IDs (**checked**, `MTL4ArgumentTable.h`). Metal snapshots the table's
bindings at each draw or dispatch, but reads the bytes at execution: the probe rewrote one
dispatch's root data after recording and the GPU saw the new value (**checked**). So each
`cmdSetRootData` must copy its bytes to command memory that lives until the GPU is done, and
point index 2 at it. Draw parameters need the same (§6). This is the §5 decision.

### 3.4 Residency

Metal 4 needs every resource reached through the heaps resident through an `MTLResidencySet`.
Since every RI resource is placed in an RI `Memory` (P31) and a `Memory` is one `MTLHeap` (§4),
the device keeps one residency set of heaps, attached to the queue. Adding 256 heaps and
committing takes 1.4 ms; removing them takes 0.02 ms (**checked**). `allocateMemory` and
`freeMemory` add and remove the heap, committed before the next submission. No per-resource
residency calls are needed.

## 4. Memory (P31, P32)

A `Memory` is an `MTLHeap` of type `MTLHeapTypePlacement`, untracked (hazard tracking off: RI
owns synchronization, P36). Buffers and textures are placed at the user's offset
(`newBuffer(length, options, offset)`, `newTexture(desc, offset)`).
`bufferRequirements` and `textureRequirements` come from `heapBufferSizeAndAlign` and
`heapTextureSizeAndAlign` (placement and sizes **checked** in the probe for buffers).

| RI kind | Metal | Note |
|---|---|---|
| GPU | `StorageModePrivate` | |
| Upload | `StorageModeShared`, `CPUCacheModeWriteCombined` | |
| Readback | `StorageModeShared`, default (cached) | |
| GPU + CPU write | `StorageModeShared`, write-combined | Always available on unified memory; size = `recommendedMaxWorkingSetSize` (to verify) |

Aliasing in placement heaps follows the user's placement, as on D3D12 (to verify against Metal's
aliasing rules for textures).

## 5. Command memory: an invariant to amend (decision)

D3D12 root constants and Vulkan push constants live in command memory that the driver owns.
Metal 4 has none for bytes (§3.3), so the Metal backend needs its own **command memory per
command list**: shared-storage buffers where `cmdSetRootData` copies its bytes (up to 256 per
call, 8-byte aligned) and draws write their parameters (§6). It grows by chunks (64 KiB, then
doubling) and is recycled by `resetCommandList`, which RI already allows only once the list's
last submission is reached. So no tracking is needed, exactly like `MTL4CommandAllocator` itself.

This breaks the letter of "RI allocates no memory on its own (P31)… RI's own objects are small
and fixed". Options:

- **(a) Amend the invariant (recommended):** "except command memory, which a command list owns
  and recycles at reset, as drivers do on the other backends". No signature change; the user sees
  no difference between backends.
- (b) The user gives memory to the command list: a new `CommandListDesc` field (capacity, or a
  `Memory` range) on every backend, ignored by D3D12 and Vulkan. Recording fails when it is full.
  This changes the signature of `createCommandList` and adds a limit the other backends do not
  have.
- (c) Fixed capacity set at `createDevice`, no growth: a hidden limit on draws per list.

## 6. Commands, encoders and synchronization

### 6.1 Recording (P34)

`CommandList` = `MTL4CommandAllocator` + `MTL4CommandBuffer` + the command memory of §5 + one
`MTL4ArgumentTable` per encoder kind. Metal records through encoders: RI opens a compute encoder
for dispatches, copies, clears and acceleration-structure builds. Metal 4's compute encoder also
does blits (`copyFromBuffer`, `copyFromTexture`, `fillBuffer`). RI opens a render encoder for each
`cmdBeginRendering` section, and keeps the open encoder across commands of the same kind, so a run
of dispatches and copies is one encoder. `cmdWriteSmallData` (P107 (1)) copies from command
memory with `copyFromBuffer`. `cmdClearBuffer` uses `fillBuffer`, which fills bytes, so non-byte
patterns need a copy from command memory (to verify).

### 6.2 Barriers (P36)

Metal 4 barriers are by stage, with visibility options, at queue level (between encoders) or
encoder level (within one). The calls are `barrierAfterQueueStages`,
`barrierAfterEncoderStages` and `barrierAfterStages` (**checked**, `MTL4CommandEncoder.h`).
`cmdBarrier(before, after)` maps as follows:

| RI `Stage` | `MTLStages` |
|---|---|
| `eCopy` | `Blit` |
| `eCompute` | `Dispatch` |
| `eIndirect` | `Dispatch \| Vertex \| Object \| Mesh` (Metal reads indirect arguments at the consuming stage) |
| `eVertex` | `Vertex \| Object \| Mesh` |
| `ePixel`, `eRenderTarget`, `eDepthStencil` | `Fragment` (attachments are fragment work in Metal) |
| `eAccelerationStructure` | `AccelerationStructure` |
| `eAll` | `All` |

RI records barriers as it meets them. A barrier between two encoders becomes a consumer barrier
(`barrierAfterQueueStages`) at the start of the next encoder. A barrier inside an open encoder
becomes `barrierAfterEncoderStages`, with visibility `Device`. Inside a rendering section,
Apple's tile-based GPUs cannot wait on fragment work before vertex work in the same pass (to
verify): validation builds report such a barrier, and the user ends the section.

RI's layout switches have no Metal meaning (no layouts). Metal 4 has no per-resource transitions,
and texture compression state is managed by Metal.

### 6.3 Submission and timeline (P20)

`Queue` = `MTL4CommandQueue` with the residency set. `submit` commits, then signals the device's
`MTLSharedEvent` with the next value; `wait` and `isReached` use `waitUntilSignaledValue` and
`signaledValue`. GPU errors arrive through `MTL4CommitFeedback` (P53 (2)).

## 7. Indirect execution (P48)

MSC vertex shaders read the draw arguments from buffer index 4 (`IRRuntimeDrawArgument` or
`IRRuntimeDrawIndexedArgument`, the same layouts as D3D12's and Vulkan's), and the index type from
index 5 (**checked**, runtime header). MSC 4.0's own Metal 4 helpers take a caller-provided
scratch buffer for these (`IRRuntime4DrawGetScratchBufferSize` = 44 bytes, **checked**). This
confirms §5. RI therefore:

- **Direct draws:** writes the arguments to command memory and binds index 4. Index 5 points at a
  small fixed device buffer holding the three index-type values.
- **Level 1 without a count:** encodes `maxCount` indirect draws, each binding index 4 to the
  record itself (`args + offset + i·stride`) and reading its arguments from the same address.
  Dispatches use `dispatchThreadgroups(indirectBuffer, …)`, mesh draws
  `drawMeshThreadgroups(indirectBuffer, …)`: Metal's layouts equal RI's (to verify on the GPU).
- **Level 2 without a count:** RI's record is `[root data][arguments]` and MSC's top-level
  argument buffer is the root constants at offset 0. So record *i* binds index 2 to the record and
  index 4 to the record + root data size: **no copy, no GPU work**. `indirectScratchRequirements`
  returns 0.
- **A GPU-written count** has no direct Metal equivalent (no draw-count indirect). Two ways:
  - **(a)** an RI kernel that writes an indirect command buffer (`MTLIndirectCommandBuffer`, with
    per-command buffer bindings at indices 2 and 4) and `executeCommandsInBuffer(icb,
    indirectRangeBuffer)` with the GPU-written count as the range;
  - **(b)** an RI kernel that copies the records into the scratch range and zeroes the instance or
    group counts of records at or beyond the count, then `maxCount` indirect draws as above.

  Both need RI's **first built-in shader**, in MSL (MSC cannot write ICB commands), compiled at
  RI's build time with `xcrun metal` and embedded. P105 (4) called that a large step. (b) is
  simpler, works for every kind including dispatches, and fits the existing scratch requirement.
  (a) costs less GPU time for large `maxCount` and is closer to D3D12's `ExecuteIndirect`.
  Recommended: (b) first, measured against (a) (P18). The CPU cost is `maxCount` encoded draws per
  call either way for (b), one call for (a).

So `DeviceLimits::indirectRootData` (Level 2) can be **true** on Metal. The first version can
ship Level 1 and 2 without counts and report counts unsupported until the kernel exists (question
Q4).

## 8. Rendering and pipelines (P38, P40, P102–P105)

- **Sections:** `MTL4RenderPassDescriptor` from `RenderingDesc` (load, store and clear map
  directly). A section without attachments uses `renderTargetWidth` and `renderTargetHeight` with
  `defaultRasterSampleCount` (to verify). Viewport, scissor, stencil reference, depth bias and
  clamp, fill mode and topology are encoder state or pipeline state, as on D3D12.
- **Pipelines:** `MTL4Compiler` builds `MTL4ComputePipelineDescriptor`,
  `MTL4RenderPipelineDescriptor` and `MTL4MeshRenderPipelineDescriptor` from the library in `MTLB`.
  It runs on any thread, which fits P106's any-thread list. The probe built pipelines from MSC
  metallibs with the Metal 3 pipeline API; the MTL4 path is to verify.
- **Formats:** Apple silicon has no D24 depth; RI's `formatSupported` reports it. The full format
  table comes with the texture milestone.
- **Copies (P108):** `copyFromTexture` and `copyFromBuffer` with bytes-per-row, as `copyLayout`
  already reports. Metal's row-pitch alignment rules are checked against `copyLayout`'s.
- **Swapchain (P43):** `CAMetalLayer` drawables. metal-cpp has `QuartzCore/` for the layer and
  drawable. Getting the layer from the tests' GLFW `NSWindow` (`glfwGetCocoaWindow`) needs a few
  lines of Objective-C++ in the tests (P109 keeps GLFW out of RI). Swapchain tests skip when no
  window server session is usable.

## 9. Features and `DeviceLimits`

| Feature | Metal 4 on Apple silicon | Proposed |
|---|---|---|
| Mesh and amplification (task) shaders | Object and mesh functions; RI's test shaders convert | supported |
| Ray queries (P49) | Every Apple-silicon GPU; hardware on M3+ (Apple9). `supportsRaytracing` = 1 on the M4 (**checked**) | supported, plus a new fact `rayTracingHardware` (name to agree; §2 of the handoff) |
| Acceleration structures | Metal acceleration structures; MSC binds them through a 64-byte `IRRaytracingAccelerationStructureGPUHeader` in the heap entry (**checked** size). RI's shared 64-byte instance format against Metal's instance descriptors: to verify | supported if the instance format holds; otherwise RI's helper converts (question Q5) |
| Geometry, hull and domain shaders | No native stages. MSC 4.0 converts them for its emulation pipeline (`--enable-gs-ts-emulation`, `IRRuntime4Draw…Emulation` helpers) | unsupported now (`geometryShaders`, `tessellation` false); emulation later |
| 64-bit atomics, groupshared | Every operation converts (Apple8, Apple9), but Metal's compiler then fails at pipeline creation (**checked**, M4, MSC 4.0b3) | `int64SharedAtomics` false (P116 (2), amended 2026-11-09) |
| 64-bit atomics, heap buffers | Only Min, Max, CompareExchange and CompareStore convert; Add, And, Or, Xor and Exchange are refused (**checked**) | `int64BufferAtomics` false; question Q3 |
| Work graphs (P50) | None | unsupported, as on Vulkan |
| Pipeline statistics (P105) | Metal 4 counter heaps have timestamps only (**checked**) | `ePipelineStatistics`, `eMeshPipelineStatistics`, `eMeshPrimitives` unsupported |
| Timestamps | `MTL4CounterHeap` + `writeTimestampWithGranularity` | supported (granularity to verify) |
| Occlusion | Visibility result buffer, set per render pass | supported; the section must know the pool, so occlusion queries inside one section use one pool (a new rule for every backend, or a Metal-only check; question Q7) |
| Wave size | MSC requires 32 (`WaveGetLaneCount()` = 32) | nothing to report: RI has no wave-size fact today |
| Not in MSC | `SV_ViewID`, VRS, sampler feedback, `SV_StencilRef`, pack/unpack intrinsics | unsupported (RI does not expose them today) |

## 10. Validation and debugging (P21, P53)

- **Native validation:** documented, not set by RI (the user's choice). The macOS presets' test
  environment sets `MTL_DEBUG_LAYER=1` and `MTL_SHADER_VALIDATION=1` in debug presets.
  `docs/site/docs/concepts/validation.md` says what each one catches.
- **RI's own checks** (P88) run unchanged; Metal-only rules go into `src/RI/Validation/` like the
  others.
- **Names and markers:** `setLabel` on every object at creation (P106 (2)); debug groups on the
  command buffer for `cmdBeginMarker` and `cmdEndMarker`. GPU capture through `MTLCaptureManager`
  under native access.

## 11. Native access (P22, P84)

`<RI/Metal/Native.hpp>` returns borrowed `MTL::` pointers: `MTL::Device *`,
`MTL4::CommandQueue *`, `MTL::Heap *`, `MTL::Buffer *`, `MTL::Texture *`,
`MTL4::CommandBuffer *`, and the open encoder for native sections (P70). Objective-C users cast
them to `id<MTL…>` with `(__bridge id)`. Internally, RI keeps `void *` and retains and releases
explicitly, as D3D12 keeps COM pointers.

## 12. Later: a Metal 3 path

Kept possible by the design: heaps, `IRDescriptorTableEntry` heaps, the fixed root signature and
command memory are the same on Metal 3. There, root data could use `setBytes`, residency
`useHeap`, and barriers `memoryBarrierWithScope` with fences across encoders. Metal 3 would add
only Intel Macs (macOS 26 and older).

## 13. Plan

1. **Pins and build:** metal-cpp submodule, `cmake/EquineMSC.cmake` (`EQUINE_MSC_DIR`),
   `EQUINE_METAL_BACKEND` (Apple only), `equine_add_shaders`' `.mtl.dxil` output, a
   `macos-debug` test environment with the Metal layers.
2. **Milestone 2 path:** device, memory, buffers, heaps, the fixed root signature, a compute
   pipeline, dispatch, readback: `samples/compute_readback` and `tests/gpu/ComputeTests.cpp`, then
   `tests/conformance/` with the third backend.
3. Then by `docs/Plan.md`'s milestones: textures and samplers, rendering, indirect, mesh, ray
   queries, queries, swapchain.

Logs go to `docs/measurements/macos/verify/<commit>/` on the verify branches, never to master.

## Decisions and proposed solutions

### Agreed by the user (2026-11-08)

- **Q1 (§2.2):** Metal bytecode is a DXIL container plus an `MTLB` part holding the metallib.
- **Q2 (§5):** each command list owns command memory that grows by chunks and is recycled at
  `resetCommandList`. P31's invariant is amended for command memory.
- **Q3 (§9):** `int64BufferAtomics` is false on Metal. The flag keeps one meaning on every backend
  (all operations); `int64SharedAtomics` is true (amended 2026-11-09: false, see P116 (2)).
- **Q4 (§7):** GPU-written counts use RI's first built-in kernel (MSL): it copies the records into
  the scratch range and zeroes the counts of records at or beyond the count. ICB is measured
  against it later (P18). Until the kernel exists, calls with a count are reported unsupported
  on Metal.

### Proposed and agreed by the user (2026-11-08)

**Q5. Acceleration-structure instances: keep RI's 64-byte format, and RI converts it on the
GPU.** Checked in the SDK and MSC headers:

| | RI (`AccelerationStructureInstance`, D3D12's) | Metal 4 (`MTLIndirectAccelerationStructureInstanceDescriptor`) |
|---|---|---|
| Size | 64 bytes | 72 bytes |
| Transform | `float[3][4]`, row-major | `MTLPackedFloat4x3`; row-major allowed by `instanceTransformationMatrixLayout = RowMajor` on the MTL4 build descriptor |
| ID, mask | `instanceId : 24`, `mask : 8` in one word | `userID` (32 bits), `mask` (32 bits) |
| Hit-group offset | `contributionOffset : 24` | `intersectionFunctionTableOffset` (unused by ray queries) |
| Flags | `flags : 8`: 1 cull off, 2 front counter-clockwise, 4 force opaque, 8 force non-opaque | `options`: the same four bits with the same values |
| Bottom level | `uint64 blasAddress` | `MTLResourceID accelerationStructureID` (64 bits) |

Instances may be written by the GPU, so the conversion runs on the GPU:
- `AccelerationStructure::gpuAddress` on Metal is the bottom level's `gpuResourceID`. It is the
  value instances name; the field's comment already says so, and no code reads it as an address.
- `cmdBuildTlas` runs a second built-in kernel (with Q4's, in the same embedded metallib). It
  repacks each 64-byte record into a 72-byte Metal record and builds from those. The bits copy
  unchanged; `contributionOffset` is dropped.
- The repacked records live in the build scratch: `tlasSizes` on Metal adds `72 × instanceCount`
  (aligned) to `buildScratchSize` and `updateScratchSize`. No signature changes, and the portable
  format stays.
- MSC reads a top level through a 64-byte `IRRaytracingAccelerationStructureGPUHeader` (§9),
  whose address is in the heap entry. RI keeps it in the structure's own range:
  `accelerationStructureRequirements` adds 64 bytes plus one 4-byte instance contribution, all
  zero (ray queries do not use them). `writeDescriptors` of an `AccelerationStructureView` writes
  the header on the CPU (shared storage) and the entry.

**Q6. Out-of-bounds access stays undefined on every backend, and no `--boundsCheck`.** Vulkan
already turns robustness off (`backends/vulkan/Device.cpp:179`: "Robustness costs performance
just by being on"), so RI defines no out-of-bounds result today. Clamping only in validation
builds would make debug and release builds compute different results, which hides bugs. Instead,
`MTL_SHADER_VALIDATION` (set in the debug presets, §10) is the Metal counterpart of P53 (4)'s
GPU-AV and GPU-based validation. What it reports for heap-indexed views is to verify; the
concepts page states the rule ("out of bounds: undefined on every backend").

**Q7. Occlusion queries: no new rule, through command memory.** Metal counts samples into the
render pass's visibility result buffer, which must be known when the section begins, before RI
sees `cmdBeginQuery`. So:
- Once the device has an occlusion pool, each `cmdBeginRendering` points the pass's
  `visibilityResultBuffer` at a page of the command list's command memory (§5): 4096 slots of
  8 bytes.
- `cmdBeginQuery(pool, i)` takes the next slot (`setVisibilityResultMode(Counting, slot)`), and
  `cmdEndQuery` sets the mode back to disabled.
- At `cmdEndRendering`, RI copies each used slot to the pool's buffer (one `copyFromBuffer` per
  query, after the encoder ends). `cmdResolveQueries` copies from the pool as for timestamps.
- Validation builds report more than 4096 occlusion queries in one section, a limit far above
  use. Any number of pools can be used in one section, as on D3D12 and Vulkan.

**Q8. `DeviceLimits::rayTracingHardware` (bool).** True when ray queries run on dedicated
hardware: M3 and later on Metal (`supportsFamily(Apple9)` with `supportsRaytracing`; which
family check is exact is to verify on M1 or M2 data, since the M4 says yes to both), and always
true on D3D12 and Vulkan (RI's floor, P3, is Turing and RDNA2, all hardware). `rayTracing` keeps
its meaning: ray queries are available.
