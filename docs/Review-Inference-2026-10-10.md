# Review: softening "RI infers nothing" (2026-10-10)

Status: **applied to CONTEXT.md on 2026-10-10** (P36, P45, P62, P67, P68, P71; S4 settled). Decisions taken during the review
are recorded per item below and applied to the P rows at the end.

## The request

P36 says "RI infers nothing". The user finds this too harsh. Proposed new rule:

> RI prefers not to infer. It infers when the answer is **evident** (RI can know it for
> certain, with no tracking of past commands), or when inferring **greatly simplifies the
> API**. An explicit way to override the inferred value stays available.

Reading confirmed by the user: "we check" means **(a) RI works it out and does it**
(inference), not validation.

Test applied to every decision:
1. Is there something RI currently makes the user state that RI could know for certain?
2. Or would inferring it remove a whole step from the normal use of the API?
3. Does inferring it need tracking across commands, hidden caches, or guesses? If yes, it
   stays explicit (P15, P34: no hidden state).

## A. Candidates: decisions that would change

### A1. P68: the "after" half of RI's own layout switches

- Today: every switch RI emits (D3D12, RDNA2 Vulkan mode T) waits for **all** earlier work
  and blocks **all** later work.
- Evident part: RI always knows which operation comes **right after** the switch, because
  it is RI's own operation (rendering, copy, clear, resolve, present). So the "after" half
  can name exactly that operation instead of "everything".
- The "before" half stays full: knowing it needs the user's past commands (tracking).
- Effect: about half of the overlap loss disappears, with no hidden state. The
  measurement (P79) then measures this version.
- Verdict: **change** (evident).
- **Decided (2026-10-10).** RI knows its own operation exactly, so the half of each switch
  that touches that operation is exact, and the other half stays full:
  - switch **into** an RI operation (resting → render target, copy, clear, resolve,
    present): "before" = all earlier work, "after" = exactly that operation;
  - switch **back** to resting after it (e.g. at `endRendering`): "before" = exactly the
    operation that just ran, "after" = all later work (RI cannot know the next user).
  The second bullet was added after the user's "ok" as the mirror case of the same rule.
  The L1 measurement (P79) measures this version.

### A2. P71: resetting queries

- Today: `resetQueries` is required before every reuse on Vulkan.
- Inferred version: RI resets every query when the pool is created (host reset, Vulkan
  1.2, on the floor) and resets the resolved queries right after `resolveQueries`. In the
  normal flow (write → resolve → read → write again) the user never calls reset.
- Removes a Vulkan-only step that does nothing on D3D12: a large simplification.
- Cost: a query resolved twice gives garbage the second time (documented; validation can
  report it). `resetQueries` stays available as the explicit form.
- Verdict: **change** (simplifies the API greatly).
- **Decided (2026-10-10): CPU (host) reset.** The pool is host-reset at creation. After
  reading results (after the timeline point), the user calls `pool.reset(first, count)`, a
  CPU call (`vkResetQueryPool`, Vulkan 1.2) that is a no-op on D3D12; no GPU cost. GPU
  auto-reset after resolve was rejected (small GPU cost per resolve). The recorded
  `resetQueries` command stays for resets in the middle of a command list. A "read results"
  helper may do read + reset.
- Background given to the user: resolve = GPU copy from query storage into a buffer
  (D3D12 `ResolveQueryData`, Vulkan `vkCmdCopyQueryPoolResults` with 64-bit + wait flags);
  neither empties the queries; Vulkan alone can also read results on the CPU directly
  (`vkGetQueryPoolResults`, native only, P22).

### A3. P50 / P62: work-graph backing-memory initialization

- D3D12 needs an `INITIALIZE` flag the first time a program is used with a given backing
  memory. No decision says who sets it (review R32, point 2).
- Evident: RI sees every `dispatchGraph` and knows whether this (program, backing memory)
  pair was used before. That is per-object state, not command tracking.
- Explicit override: a flag to force re-initialization.
- Verdict: **add** (evident).
- **Decided (2026-10-10).** Each program object stores the backing memory it was last
  dispatched with. RI sets `INITIALIZE` on the first dispatch and whenever the backing
  memory changes, and leaves it off otherwise. `dispatchGraph` takes an optional "force
  initialize" flag to reset the graph's scratch state deliberately.

### A4. P45 / P67 (and S4): D3D12 root signature without `[RootSignature]`

- Today: P67 says a D3D12 shader without `[RootSignature]` is a readable error. This
  contradicts P23 ("layouts derive from reflection by default").
- Inferred version: with no `[RootSignature]`, RI builds the root signature from the
  shader itself (heap-indexing flags, plus root constants from the one small constant
  buffer marked for root data). The D3D12 marking would become optional, and only
  `[[vk::push_constant]]` (which DXC needs anyway) would remain. This addresses the user's
  discomfort with the markings (S4).
- Open: reading constant-buffer sizes from DXIL may need `dxcompiler.dll` at runtime,
  which P67 allows only in validation builds. **Needs research** (DXIL container parts,
  PSV0/RDAT) before deciding.
- Verdict: **candidate, needs research first.**
- **Decided (2026-10-10): keep the markings; no inference.** Reason (user): RI must not
  load `dxcompiler.dll` at runtime, because users must not be forced to have HLSL
  compilation available at runtime (consistent with P67). Inferring the root-data size
  without it is not possible (the size lives only in the full reflection data). Fixed
  maximum size and a C++ `rootDataSize` were dropped.
- **Improved P45 helper**, tested with DXC 1.8.2502 (Vulkan SDK 1.4.309):
  ```hlsl
  #ifdef __spirv__
  #define EQ_SIZE_CHECK(Type, count)
  #else
  #define EQ_SIZE_CHECK(Type, count)       _Static_assert(sizeof(Type) == (count) * 4, "EQ_ROOT_DATA: count does not match sizeof(" #Type ") / 4");
  #endif
  #define EQ_ROOT_SIG_ATTR(count)       [RootSignature("RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED), "                      "RootConstants(num32BitConstants = " #count ", b0)")]
  #define EQ_ROOT_DATA(Type, name, count)                                   [[vk::push_constant]] ConstantBuffer<Type> name : register(b0);       EQ_SIZE_CHECK(Type, count)                                            EQ_ROOT_SIG_ATTR(count)
  #define EQ_ROOT_SIGNATURE(count) EQ_ROOT_SIG_ATTR(count)
  ```
  - `EQ_ROOT_DATA(RootData, root, 4)` goes right before the first entry point; its
    trailing `[RootSignature]` attaches to that function. Each further entry point in the
    same file needs `EQ_ROOT_SIGNATURE(4)`; a missing one is a readable error on D3D12
    (P45). Vulkan is unaffected (the marking is on the variable).
  - The count is typed once per macro and **checked at compile time** against `sizeof`
    on the D3D12 build. The check is skipped for SPIR-V because this DXC rejects
    `_Static_assert` there ("decl type StaticAssert unimplemented").
  - Tested: `sizeof` cannot enter the root-signature string; `static_assert` is not
    accepted, `_Static_assert` is; a global `GlobalRootSignature` is silently ignored for
    `cs_6_6`; `-rootsig-define` works but needs a compile flag and an external
    `#define`, so it was dropped in favour of the single macro (user's idea).
  - S4 stays open but this is the current answer to it.
- **Iteration 2 (user: extra entry points must not repeat the size).** Tested: a count
  `#define` named after the struct (`RootData_ROOT_COUNT`) lets `EQ_ROOT_SIGNATURE(RootData)`
  take the type name only (works, checked by `_Static_assert`). User: "not good enough,
  dig more".
- **Iteration 3 findings (2026-10-10):**
  - The root-signature grammar takes integer literals only (`num32BitConstants = 2+2` →
    "Unexpected token '+2'"), so no preprocessor arithmetic can help.
  - **The constant-buffer size is in the DXIL part itself**, in the `!dx.resources`
    metadata (`!{i32 0, %root* undef, !"", i32 0, i32 0, i32 1, i32 32, null}`: space 0,
    register 0, range 1, **32 bytes**), and it survives `-Qstrip_reflect` (the driver needs
    it). Reading it does not need `dxcompiler.dll`, only an LLVM bitcode reader for the
    DXIL part (DXIL is frozen on LLVM 3.7 bitcode).
  - The heap-indexing flags are in the `SFI0` part and the bindings in `PSV0`: both are
    small, documented, fixed-layout parts.
  - Prior art: dxil-spirv (Hans-Kristian Arntzen / Valve, MIT, used by vkd3d-proton)
    ships its own minimal LLVM bitcode parser with no LLVM dependency.
  - Consequence: **inference without `dxcompiler.dll` is possible.** RI reads SFI0 + PSV0
    + the DXIL `dx.resources` metadata and builds the D3D12 root signature itself at
    pipeline creation. The HLSL keeps only `[[vk::push_constant]]`. Cost: a small
    in-house DXIL metadata reader (or a vendored parser), the D3D12 twin of SPIRV-Reflect.
    Needs a rule for which constant buffer is the root data on D3D12, where
    `[[vk::push_constant]]` is ignored (e.g. the only constant buffer, or the one at
    b0 space0).
- **Iteration 4 (user: "good, but I would prefer a macro to mark a specific constant
  buffer as the root data").** Proposed and tested marking macro:
  ```hlsl
  #ifdef __spirv__
  #define EQ_ROOT_DATA(Type, name) [[vk::push_constant]] ConstantBuffer<Type> name
  #else
  #define EQ_ROOT_DATA(Type, name) ConstantBuffer<Type> name : register(b0, space999)
  #endif
  ```
  - The marking on D3D12 is a **reserved register space** (999 here, a constant of the
    helper; not in D3D12's system-reserved range 0xFFFFFFF0+). It survives
    `-Qstrip_reflect` in `dx.resources` (`i32 999, i32 0, i32 1, i32 16`: space, register,
    range, 16 bytes), while names do not (they are empty in the DXIL part).
  - Other constant buffers in the same shader are untouched (tested with a second
    `ConstantBuffer<Material>`: space 0, ordinary descriptor on both backends).
  - Every entry point in the file works with no extra line (tested `mainA`, `mainB`).
  - No `[RootSignature]`, no count, no compile flag; no "attribute ignored" warning
    (the Vulkan attribute is emitted only for SPIR-V).
  - RI builds the D3D12 root signature from SFI0 + PSV0 + `dx.resources` with its own
    DXIL reader.
- **Iteration 5 (user: "ideally no reserved space; other options?").** In the stripped
  DXIL part, resource *names* are empty, but the LLVM **type name** of a constant buffer
  survives (bitcode type table), including with `-Qstrip_reflect` and `-O3`. Options:
  1. **Reserved register space** (iteration 4). Robust: spaces are part of the documented
     binding model.
  2. **Helper-chosen block name** (tested):
     ```hlsl
     #ifdef __spirv__
     #define EQ_ROOT_DATA(Type, name) [[vk::push_constant]] ConstantBuffer<Type> name
     #else
     #define EQ_ROOT_DATA(Type, name) cbuffer EquineRootData { Type name; }
     #endif
     ```
     D3D12 metadata: `!{i32 0, %EquineRootData* undef, !"", i32 0, i32 0, i32 1, i32 16,
     null}`; the buffer gets an ordinary auto-assigned register (b0 here, `material` b1).
     User code is identical on both backends (`root.scale`). No register or space is
     reserved, only the identifier `EquineRootData`. Risk: LLVM type names are a DXC
     implementation detail, not a documented guarantee. (`[[vk::push_constant]]` cannot
     go on a `cbuffer` block: "only applies to global variables of struct type", hence the
     per-backend form.)
  3. **No marking on D3D12**: the only declared constant buffer is the root data; more
     than one is a readable error. Robust, but ordinary constant buffers can no longer
     sit next to root data.
- **Final A4 decision (2026-10-10): option 1, reserved register space**, after the user
  asked about its costs: none at runtime (spaces are binding-model labels; drivers see
  the same root constants), no compile restriction (SM 5.1+, values below 0xFFFFFFF0),
  nothing on Vulkan. Only cost: the user cannot use that space; RI reports a readable
  error if something else is declared there. This **supersedes the earlier "keep the
  markings" decision**: RI reads the D3D12 shader itself (SFI0, PSV0, DXIL
  `dx.resources`) with its own small DXIL reader and builds the root signature; no
  `[RootSignature]`, no count, no `dxcompiler.dll`. The helper is the iteration-4 macro.
  Library (work-graph) shaders: to check at implementation whether the same reading
  applies (P62 currently says `GlobalRootSignature`).

## B. Already consistent: RI already infers here

These follow the softened rule as written today; nothing to change.

- P63: the acquire wait and the present switch on Vulkan.
- P65, P70: restoring the user's pipeline and root data after RI's own clear and after
  native sections.
- P60: Vulkan enables every supported feature.
- P43: a default swapchain image count for the present mode.
- P23: pipeline layouts from reflection (but see A4).
- P73: every texture has a default whole-texture view.

## C. Considered, and staying explicit

- **P36, barriers between user work.** With bindless, RI cannot see which resources a
  shader touches, so it cannot know which barrier is needed. Not evident. Barrier tracking
  stays a helper (P15, P35). Only the wording of P36 changes.
- **P69, discard.** RI could mark a texture as "never used" at creation, but aliased
  textures take over memory again every frame. Knowing *when* needs tracking of other
  resources. Stays explicit; validation still reports a missing discard.
- **P75, read-only depth.** At `beginRendering` RI cannot know whether later draws will
  write depth (pipelines are bound afterwards). Stays explicit.
- **P58 / P74, usage flags and view formats.** Needed at creation, before any use. Not
  knowable. Stays explicit, with the helper.
- **P38, render target formats in pipelines.** Not knowable at pipeline creation. Stays
  explicit, with the lazy helper.
- **P27, the out map of native sections.** RI cannot see native commands. Stays explicit.
- **P56 / P59 / P20, timeline rules** (reset lists, rewrite slots, destroy). Inferring
  these means deferring work behind the user's back (a frame concept). Stays helpers.

## If "we check" meant "we validate"

Then the softening is about validation (P21) rather than inference. Validation can check,
cheaply, in debug builds:
- destroying a resource, or rewriting a descriptor slot, that unfinished GPU work may use
  (P20, P59);
- resetting a command list before its timeline point (P56);
- a query resolved twice without reset (A2).

## Proposed new wording of P36 (not applied)

> Barriers in RI are explicit (user-issued), coarse stage→stage by default. RI prefers not
> to infer; it infers only what is evident without tracking past commands, or what greatly
> simplifies the API, and always keeps an explicit override. Barriers between user work are
> not evident under bindless, so automatic barriers exist only in helpers (P35).
