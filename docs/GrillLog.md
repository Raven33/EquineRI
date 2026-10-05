# Design Grill Log

A chronological record of design questions and answers. Settled outcomes are copied to
`../CONTEXT.md` §2; this file keeps the reasoning and exact answers.

## Brief (2026-10-03)

- A research RHI over D3D12 and Vulkan, with HLSL as the shading language (Slang is too
  incomplete). EquineEngineV2 is a usable reference.
- Teaching is no longer a goal.
- The shader remains the single source of truth. Helpers may ship, but the RHI/engine
  must be expressive enough that users could write their own.
- Bindless first. Bindful only if there is demand, possibly emulated through helpers.
- Inspiration: Sebastian Aaltonen, "No Graphics API"
  (https://www.sebastianaaltonen.com/blog/no-graphics-api), and
  https://www.youtube.com/watch?v=7exuYa1X0QM.
- A soft goal of easy porting from Equine (since dropped, see Q8).

## Round 1 (2026-10-03)

**Q1. Where does the core sit: a thin explicit layer, or an Equine-style compiler?**
Higher level than current APIs. Sending work to the GPU must be trivial, while users keep
enough control to get 90% of what a native API offers. Don't hide too much; the target
is research velocity.

**Q2. Pointers or (index, offset) addressing in shaders?**
Too much help for the base system. Something like this may ship later as a helper, but it
is not the base API. Aaltonen is not the target, only potential inspiration.

**Q3. Hardware floor and platforms?**
Linux and Windows. AMD and NVIDIA GPUs. Turing and RDNA2 are acceptable floors.

**Q4. What does "the shader is the single source of truth" cover?**
The base system introduces no custom DSL or annotations. Needs further discussion.

**Q5. Memory model?**
Undecided; needs further discussion.

**Q6. Synchronization granularity; lifetime ownership?**
Both coarse and fine-grained, with coarse as the default. Users must be able to architect
complex systems on top, so don't hide too much. Lifetime: undecided; the pros and cons
need discussing.

**Q7. Feature scope; policy for asymmetric features?**
Mesh + task shaders: yes. RT: inline only for now. Work graphs: yes. Indirect
execution: yes. Sparse/tiled: don't know yet. Cooperative matrix/vectors: yes. 64-bit
atomics: yes. VRS: don't know yet. Asymmetric features use a capability query, or a
graceful crash with a clear error (a crash is fine in a research setting).

**Q8. Equine porting?**
Not needed. Three papers built on various Equine states, plus a D3D12 system using work
graphs and a custom GPU virtual-memory system, serve as targets: this is the kind of
system the project should enable users to build.

**Q9. Repository and toolchain?**
New repo, keeping the Equine name (exact name to be discussed). clang-cl, C++20, code
style from the current code. Build system to be validated later.

## Round 2 (2026-10-03)

**Dependencies.** Remove unordered_dense; containers are written in-house.

**Q1. The ideal hello-world.** Not now. The user leans strongly toward raw-Vulkan-like
code and wants to consult colleagues first.

**Q2. Explicit or inferred barriers?** This raised a prior problem: the user must never
have to know about layout transitions. That has to be resolved first (L1).

**Q3. Did V2's compile/resident lifecycle help the papers?** No. The papers use Equine
purely as a graphics API. The lifecycle was scaffolding for a frame graph (designed
early 2024), which would remove barrier management but is not necessarily the best
solution.

**Q4. A universal root layout?** WE DON'T IMPOSE ANYTHING ON THE USER. Equine avoids
imposing architectural decisions as far as possible. Helpers with such behavior may
exist, but only as a layer on top, and users must be able to write their own.

**Q5.** Not covered in round 2.

**Q6. Needs of the GPU virtual-memory system?** Not relevant yet; the code will be
provided.

**Q7. Lifetime.** Explain the timeline-point idea (see below).

**Q8. The papers.** All three use mesh and compute shaders. The biggest problem was
fighting unimplemented Equine features and implementing them on the fly. A native API
would not have had that problem, but the colleagues are not familiar with one.

**Q9. Name.** EquineEngine. The user handles any git collision with V1.

### Follow-up: timeline points (M2)

Both native APIs already expose a per-queue monotonically increasing 64-bit counter: a
Vulkan timeline semaphore and a D3D12 fence. The base would surface it unchanged:

```text
TimelinePoint p = queue.submit(work);   // {queue, value}, signalled when the work completes
bool done = device.isComplete(p);       // non-blocking CPU query
device.wait(p);                         // blocking CPU wait
queue.submit(work, waitFor = {p});      // GPU-side cross-queue wait
device.destroy(resource);               // immediate; the caller guarantees no GPU use
```

There is no frame concept and no hidden deferral. Policies become helpers built on points:

- a deferred-destruction queue: `retire(resource, p)`, then `collect()` destroys
  everything whose point has completed;
- frames in flight: a ring of N points that the application waits on;
- bindless slot recycling, staging/upload ring reuse, readback readiness, and any user
  sub-allocator that must know when the GPU has finished with a memory range before
  reusing it all key off the same points.

The cost is that the bare `destroy` is a footgun. Under bindless, the base cannot know
whether a shader still reads a resource. Debug validation can only be conservative, for
example flagging a destroy while any submission issued after the object's last
`retire`/creation is still pending.

### Follow-up: hiding image layouts (L1)

Constraints: layouts are per-image (sub)resource state. Under bindless, the base cannot
see which textures a shader samples or writes. Vulkan global memory barriers cannot
change layouts. P15 rules out mandatory tracking.

Layouts only matter for operations the base itself performs, plus shader access:

| Use | Vulkan | D3D12 (enhanced barriers) |
|-----|--------|---------------------------|
| Sampled / storage shader access | `GENERAL` valid for both | layout requirements to verify (`COMMON` / `SHADER_RESOURCE` / `UNORDERED_ACCESS`) |
| Color / depth attachment | `GENERAL` valid; possibly slower (compression) unless `VK_KHR_unified_image_layouts` | `RENDER_TARGET`, `DEPTH_STENCIL_*` required |
| Copy / clear / resolve | `GENERAL` valid | copy layouts or `COMMON` |
| Present | `PRESENT_SRC` required | `PRESENT` / `COMMON` |

Candidates:

- **A. A resting layout plus base-managed transitions around visible operations.** Every
  texture rests in a single shader-access layout. The base moves it into attachment,
  copy, or present layouts only around the operations it executes itself (render-pass
  begin/end, copy, clear, resolve, present), then back. User barriers become pure
  execution/memory dependencies, coarse or per-resource. No per-resource state is
  tracked beyond a static resting layout. Risk: returning attachments to the resting
  layout after each pass may cost decompression on some hardware. Measure this on
  Turing and RDNA2 before committing.
- **A'. A with unified layouts.** Where `VK_KHR_unified_image_layouts` is supported,
  Vulkan stays in `GENERAL` throughout and the around-operation transitions vanish.
  D3D12 keeps A's attachment transitions. Verify driver coverage on the hardware floor.
- **B. Access-described barriers (fine-grained form).** A per-texture barrier names
  accesses (`ColorAttachmentWrite → FragmentSampled`), never layouts, and the backend
  derives the layout. This fits as the fine-grained variant of P10.
- **C. Simultaneous-access textures.** Always valid and transition-free, but they disable
  compression and are not available for depth. Possibly useful as an opt-in.
- **D. Base-side state tracking.** Rejected: it violates P15, and bindless hides shader usage.

Current proposal: A/A' at the base, with B as the fine-grained barrier form, gated on a
measurement spike for decompression cost.

## Round 3 (2026-10-03)

- P15 softened: "the base makes as few architecture decisions for the user as possible."
- P17 softened: limit external dependencies; before pulling one in, evaluate whether a
  simpler in-house solution would do (for example, containers).
- Name: since the base has no frames in flight or double/triple buffering, it is renamed
  **EquineRI**. The timeline-point proposal was accepted.
- The user asked why a virtual-memory allocator was mentioned. It was only an example of a
  user sub-allocator that needs to know when GPU memory can be reused. The example was
  dropped from the notes as speculative.
- L1: this is a high-performance system. No architecture choice that can affect
  performance is made without careful joint evaluation. The candidates are A' and B, but
  the user wanted to see what each looks like → `docs/LayoutEvaluation.md`.
- Desktop and laptop only; no mobile.
- Q2: validation is a compile-time flag enabling native validation and Equine's own checks.
- Q3: there must always be a way to reach native resources (for example, for a
  bleeding-edge feature Equine does not support yet).
- Q4: a/d — reflection-derived layouts, possibly alongside other sources.
- Q5: immediate versus deferred recording — don't know. Multithreaded recording — yes.

## Round 4 (2026-10-03)

- Q3: C++ data is checked against shader reflection only under the validation flag (P24).
- Q4: no preference on namespaces → `RI::` (P25).
- L1: B is too wordy and very ugly → rejected (P26). The user did not follow the dense
  round-3 explanation. Explanations from here on are plain and step by step, with one
  question at a time.

## Round 5 (2026-10-03)

- L1: "Let's go." A' recorded as the direction, confirmed only after measurement.
  Driver research results are in `LayoutEvaluation.md`.

## Round 6 (2026-10-03)

**E1. Native commands mid-list?** The user proposed an in and out map so that restoring
state is easy. Sketch under discussion:

```cpp
RI::NativeScope native = cmd.beginNative();        // native command buffer + "in" information
// in: native.restingLayout(texture) gives the native layout Equine left it in
vkCmdPipelineBarrier2(native.commandBuffer(), ...); // the user's own transitions
vkCmdSomethingNew(native.commandBuffer(), ...);
cmd.endNative({{albedo, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL}}); // out: what the user changed
// Equine now transitions albedo back to its resting layout
```

The user confirmed the sketch ("Perfect") → P27. Clarification given: with
`VK_KHR_unified_image_layouts`, the resting layout is `VK_IMAGE_LAYOUT_GENERAL`. The
extension adds no layout; it is a driver promise that `GENERAL` costs nothing.

## Round 7 (2026-10-03)

**B1. One program or one per backend?** The user proposed two executables with a wrapper
on top: the wrapper creates the window and the backend fills it. Changing backend means
a full reinit anyway; there is no memory adoption.

Concern raised: sharing a window across processes is fragile. On Windows it needs a
child window reparented across processes, with focus and input quirks. On Wayland,
cross-process embedding is not generally possible. Counter-proposal: the same structure
in one process. The wrapper executable owns the window and loads `app_vulkan` or
`app_d3d12` as a shared library, each compiled against one backend with direct calls.
Switching means destroying the device, unloading, loading the other library, and
recreating on the same window. A side benefit is reloading user code without restarting
the window.

Accepted, but "this is not a main objective, do not make it a priority" → P28.

## Round 8 (2026-10-03)

**S2. When are shaders compiled?** Not a backend problem. Backends consume SPIR-V/DXIL;
a helper is provided for dynamic compilation → P29.

## Round 9 (2026-10-03)

**S3. Global heap or declared arrays?** "We don't care, equine should work with what we
give it" → P30.

## Round 10 (2026-10-03)

**M1. Who manages GPU memory?** "RI is level 2, we provide helper on top for level 1" → P31.

## Round 11 (2026-10-03)

**M3. A short list of four memory kinds?** Yes → P32.

**Mapping: always mapped?** The user asked whether only coherent memory can stay mapped,
and whether device-local memory can be mapped at all. Clarified: persistent mapping is
legal for any host-visible memory, and coherence only decides whether flush/invalidate
is needed. Device-local memory is mappable only through the ReBAR / `GPU_UPLOAD` kind.
Agreed → P33.

## Round 12 (2026-10-03)

**Y3. Immediate or deferred recording?** The user explained that V2 put the frame graph
inside RI because that drastically reduced the command API surface. A graph on top would
need either to work directly on native, or an exhaustive command API.

Answer given: that holds for a graph that *reads commands* (V2's kind). A graph that reads
*declarations* (each pass states its reads and writes) never inspects pass bodies, so it
needs only barriers, placed memory, a discard command, and possibly multiple queues.
Bindless makes the command-reading kind impossible anyway.

The user agreed it does what they want, but "really dislikes the pass abstraction" and
"despises capturing lambdas". Record it; the shape will be argued at implementation
→ P34, P35.

Consequence noted (not asked): barriers in RI are explicit; inference lives only in helpers → P36.

## Round 13 (2026-10-03)

**Y2. Multiple queues in v1?** Later → P37.

## Round 14 (2026-10-03)

**G1. Pipeline state up front, including formats?** Option 1, with an optional helper → P38.

## Round 15 (2026-10-03)

**G2. Classic vertex buffers?** No: vertex pulling and mesh shaders. Way 1 can be
retrofitted if it becomes a problem → P39.

## Round 16 (2026-10-03)

**G3. Per-target load/store?** "If we want to support MRT, we don't have a choice" → P40.
New open items R1–R9 added to CONTEXT.md for the remaining surface.

## Round 17 (2026-10-03)

**R9. Who picks bindless slots?** "Apply our control+convenience pattern" → the pattern
is named as a principle (P41), and descriptors follow it (P42).

## Round 18 (2026-10-03)

**R1. A native window handle, with GLFW in a helper?** Yes. The user asked whether this
makes users aware of double buffering. Clarified: swapchain image cycling is internal
(`acquire()` hides indices and counts), while frames in flight are a separate, opt-in
helper. `imageCount` is made optional → P43.

## Round 19 (2026-10-03)

**R2. Root data size?** "We don't choose, the user tells us via the shader; if it is not
representable, we just fail with a readable error" → P44. Follow-up opened: how the shader
marks which `cbuffer` is root data on each backend.

**Marking the root data:** native markings (`[[vk::push_constant]]` / `[RootSignature]`),
plus the optional helper macro. "Perfect, with your helper idea" → P45.

## Round 20 (2026-10-03)

**R3. Heap samplers or static samplers?** Heap samplers ("way 1 is good") → P46.

## Round 21 (2026-10-03)

**R4. Copy commands plus a layout query in RI, one-call upload/readback as a helper?** Yes → P47.

## Round 22 (2026-10-03)

**R5. Level 1 indirect always, level 2 behind a capability query?** "Go" → P48.

## Round 23 (2026-10-04)

**R6. Acceleration structures via size query, user memory, and build/update/compact
commands, with a helper?** OK → P49.

## Round 24 (2026-10-04)

**R7. Work graphs D3D12-only for now, with a clear error on Vulkan?** "Go" → P50.

## Round 25 (2026-10-04)

**R8. Query pools and commands in RI, a named-scope profiler as a helper?** Yes → P51.

## Round 26 (2026-10-04)

**G3. MSAA resolve at end of rendering, as a separate command, or both?** Both → P52.

## Round 27 (2026-10-04)

**T1. Debugging proposals (names/markers always; crash diagnostics, Vulkan printf, and
bindless checks under the validation flag; D3D12 printf later as a helper)?** "Go" → P53.

## Round 28 (2026-10-04)

**S1. Reflection + validation check in RI, shared header as a helper?** "Go" → P54.

## Round 29 (2026-10-04)

**B2. How to get the Agility SDK?** CMake downloads a pinned version → P55. Wired into the
scaffold: 1.619.6, SHA-256-verified, with an export macro and a deploy function.

## Review (2026-10-04)

A multi-agent review of all decisions found 52 confirmed issues
(`docs/Review-2026-10-04.md`). The user approved applying group A (corrections needing no
decision: stale text, factual notes, scaffold fixes). Groups B (new topics) and C
(details of decided mechanisms) remain for the next grill rounds. New open questions:
Y4, Y5, F1–F3.

## Review round B1 (2026-10-04)

**Y5. Pools and threads?** "For now no multithreaded command recording, we do research, we
don't have a lot of draw calls anyway" → P56 (supersedes Round 3 Q5). Ownership and reset
follow P41/P20 by default.

## Review round B2 (2026-10-05)

**Error model?** "I am not certain of the error model yet, for now make it easy to change"
→ P57: one central reporting channel with categories; the policy is chosen later (X1).

## Review round B3 (2026-10-05)

**Explicit texture usage flags?** "Apply the pattern, explicit + helper" → P58, with the
memory-requirements and format-support queries as default consequences.

## Review round B4 (2026-10-05)

**Descriptor heap: user-sized, default helper, no overwriting in-flight slots?** Yes → P59.
