# Metal backend: handoff (2026-11-07)

State of branch `metal-backend` and what the next session does first. Read `ARCHITECTURE.md`,
then `CONTEXT.md`, then this. Nothing about Metal is decided yet: `CONTEXT.md` has no Metal
decision, and the invariants in `ARCHITECTURE.md` §3 were written for two backends.

## 1. What exists

- **RI builds and its GPU-free tests pass on macOS** (Apple M4, macOS 27, Xcode 27.2,
  AppleClang 21): `cmake --preset macos-debug`, `cmake --build --preset macos-debug`,
  `ctest --preset macos-debug`: 45/45 with ASan and UBSan, clang-tidy clean (commit `3e3547e`).
- **DXC on macOS:** Microsoft ships no macOS DXC, so `cmake/EquineDXC.cmake` takes
  `EQUINE_DXC_DIR`, a local build of the pinned `v1.9.2609` laid out like the Linux release
  (`bin/dxc`, `lib/libdxcompiler.dylib`, `include/dxc`); the presets read it from the
  environment. DXIL built on macOS is unsigned (no libdxil), which matters only on D3D12.
  Building DXC: `cmake -C cmake/caches/PredefinedParams.cmake` with tests off, and
  `-DCLANG_FORMAT_EXE=CLANG_FORMAT_EXE-NOTFOUND` when the `clang-format` on `PATH` is not the
  version DXC's generated headers were formatted with (the source-generation check fails on
  line wrapping otherwise).
- **clang-tidy on macOS:** a standalone LLVM clang-tidy needs `-isysroot <SDK>` and
  `-stdlib++-isystem <SDK>/usr/include/c++/v1` (AppleClang's `/usr/bin/c++` adds the sysroot
  implicitly, so `compile_commands.json` carries none). On the dev Mac a wrapper on `PATH` adds
  them; RI's CMake is unchanged.
- **The Metal compiler** (`xcrun metal`, Metal Toolchain 27B47) is installed. **Metal Shader
  Converter is not**: it is a separate Apple download, needed if shaders go DXIL → Metal IR.
- No Metal code yet: no `backends/metal/`, no `EQUINE_METAL_BACKEND` option.

## 2. Decisions to settle with the user before code

Each becomes a decision row in `CONTEXT.md` (next free number after the open branches
`p110`–`p112`; check). Signatures change only with the user's agreement (§3), so a Metal
backend that cannot match a signature reports unsupported behind a `DeviceLimits` flag, as
D3D12 does for Vulkan-only features and the reverse.

1. **Metal 4 only, or Metal 3 too.** Metal 4 (`MTL4CommandBuffer`, command allocators,
   argument tables, explicit residency sets, explicit barriers, `MTL4Compiler`) maps onto
   RI's model (explicit barriers P36, user-placed memory P31, immediate recording P34) far
   better than Metal 3's hazard tracking. The dev Mac (M4, macOS 27) supports it. Recommended:
   Metal 4 only, Apple silicon only; measure before deciding otherwise (P18).
2. **Shader path** (P29, P54, P93: RI consumes bytecode, shaders are the source of truth).
   (a) DXIL → Metal Shader Converter → `.metallib`: keeps DXIL semantics, gives
   `ResourceDescriptorHeap`-style bindless through its top-level argument buffer, and has a
   reflection JSON; Apple-only tool, closed source. (b) SPIR-V → SPIRV-Cross → MSL → `metal`:
   open, but adds a translation RI does not control. Either way `equine_add_shaders` gains a
   third output, and RI reads root data and bindings from DXIL (P67) or SPIR-V as now.
3. **Backend namespace and library** following P83/P90: `RI::Metal::`, `EquineMetalBackend`,
   `EQUINE_BACKEND_METAL`, `backends/metal/RI/Metal/{Backend,Native}.hpp`. Language:
   Objective-C++ (`.mm`) or C++ with `metal-cpp`; P82's style (plain structs, free functions)
   holds either way.
4. **The mapping, area by area** (`ARCHITECTURE.md` §5 gets a third column):

   | Area | Likely Metal mechanism | Question |
   |---|---|---|
   | Memory kinds (P32) | `MTLHeap` with placement (`MTLHeapTypePlacement`) | Which kinds map to which storage modes on unified memory |
   | Descriptor heaps (P94, P99) | Argument buffers (tier 2) as the resource and sampler heaps | Layout must match the shader path's (MSC's is fixed) |
   | Root data (P44, P45) | Argument table bytes / `setBytes` equivalent | 256-byte cap (`kMaxRootDataSize`) fits |
   | Barriers (P36) | Metal 4 stage barriers | How `Stage::*` maps; RI's own layout switches have no Metal meaning (no layouts, P16) |
   | Timeline (P20) | `MTLSharedEvent` | Direct match |
   | Rendering (P40, P105) | Render pass descriptors, render encoders | Encoder boundaries vs RI's `cmdBeginRendering` sections |
   | Geometry, tessellation (P102) | No geometry shaders; tessellation is post-tessellation vertex functions | Likely unsupported behind `DeviceLimits` |
   | Mesh shaders | Object and mesh functions | Payload and limits |
   | Ray tracing (P49) | Acceleration structures, intersection queries | Ray query only, as now |
   | Work graphs, DGC (P48, P50) | Indirect command buffers (Level 2?); no work graphs | Unsupported or partial |
   | Swapchain (P43) | `CAMetalLayer`, drawables | The tests' GLFW window (P109) gives an `NSWindow` |
   | Validation (P21) | `MTL_DEBUG_LAYER`, shader validation (`MTL_SHADER_VALIDATION`) | Turned on through the environment at device creation, or documented |
   | Debugging (P53) | Labels, debug groups, GPU capture | `applyName` equivalent |

5. **Verification and sessions.** A macOS session builds and runs the suite on the Mac, as
   the `windows-ri` and `linux-ri` sessions do for theirs; logs go to
   `docs/measurements/macos/verify/<commit>/`, never to master. `tests/conformance/` gains
   the third backend.

## 3. Suggested first steps (after §2.1–§2.3 are agreed)

1. Write `docs/FeatureProposal-Metal-<date>.md` with the §2 answers, for the user's review.
2. Add `EQUINE_METAL_BACKEND` (Apple only), `backends/metal/` with device creation, a
   buffer, a compute pipeline from one test shader, dispatch and readback:
   `samples/compute_readback` and `tests/gpu/ComputeTests.cpp` are the target, the same path
   Milestone 2 took (`docs/Plan.md`).
3. Extend `equine_add_shaders` for the chosen shader path, then `tests/conformance/`.
4. Then area by area, in the order of `docs/Plan.md`'s milestones.

## 4. User's answers (2026-11-07, before the proposal)

Not yet decision rows: they go into `docs/FeatureProposal-Metal-2026-11-07.md` for review, then
`CONTEXT.md` as P113 onwards (`p110`–`p112` are taken by open branches).

- **API:** Metal 4 first, Apple silicon (M1+); a Metal 3 path later, so keep it possible.
- **Platform:** macOS only for now.
- **Shaders:** DXIL → Metal Shader Converter → `.metallib`. MSC is extracted into `~/dev/tools`
  (`pkgutil --expand-full`), not installed system-wide. Its download needs an Apple ID, so the
  user fetches the pkg. Pinned: **MSC 4.0 beta 3** (`metal-irconverter` 4.0.3, pkg SHA-256
  `449833211e4ef0d46bfaa94848e801acff49e226d3c348bba0673f13716be622`), the user's choice over
  the last stable 3.x (2025-12-05); laid out in `~/dev/tools/msc/{bin,include,lib,docs}`.
- **Language:** C++ with metal-cpp (pinned `release/metal-cpp_macOS27.0.1_iOS27.0.1`, on GitHub,
  so a submodule like Vulkan-Headers; it has all 30 `MTL4*` headers). Objective-C++ only to fill
  an API that metal-cpp lacks. `Native.hpp` exposes `MTL::` pointers.
- **Naming:** as §2.3 (`RI::Metal::`, `EquineMetalBackend`, `EQUINE_BACKEND_METAL`,
  `EQUINE_METAL_BACKEND`).
- **Validation:** documented only. RI sets no environment; presets or ctest set
  `MTL_DEBUG_LAYER` and `MTL_SHADER_VALIDATION`.
- **Geometry, tessellation:** unsupported (`DeviceLimits` false) for now; emulation revisited
  later.
- **Indirect Level 2 (P48):** the proposal investigates a mapping onto indirect command buffers.
- **Ray queries:** supported on every Apple-silicon GPU, plus a fact that says whether they are
  hardware-accelerated (M3+).
- **Swapchain tests:** skipped when no usable window server (GUI session) is available; the
  other GPU tests run offscreen.
- **Next:** check MSC's argument-buffer layout and root constants against P94/P99 and the
  256-byte root data, then write the proposal and stop for review.

## 5. Stage 1 done (2026-11-08): pins and build

- metal-cpp submodule at `third_party/metal-cpp` (`8cdde68`); `backends/metal` builds
  `EquineMetalCpp` (the private implementation, once) under `EQUINE_METAL_BACKEND`.
- `cmake/EquineMSC.cmake`: `EQUINE_MSC_DIR` (presets read the environment; `~/dev/tools/env.sh`
  exports it), version 4.0.3 checked at configure time, the fixed root signature written to
  `<build>/equine/msc_root_signature.json`, and the `equine_metal_pack` tool
  (`tools/metal_pack/MetalPack.cpp`) that appends the `MTLB` part.
- `equine_add_shaders` writes `<name>.mtl.dxil` (metallibs in `<build>/shaders/metal/`), with
  `NO_METAL` for the misuse inputs and `atomic64`.
- `macos-debug` turns on the Metal backend and sets `MTL_DEBUG_LAYER` and
  `MTL_SHADER_VALIDATION` for ctest. GPU-free suite: 46/46, with the new `dxil.metal_container`.
- **Next (stage 2):** `EquineMetalBackend` with device, memory, buffers, heaps, a compute
  pipeline from `.mtl.dxil`, dispatch and readback (`samples/compute_readback`,
  `tests/gpu/ComputeTests.cpp`), then `tests/conformance/` with the third backend.
