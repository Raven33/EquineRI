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

## Review round B5 (2026-10-07)

**Device creation.** The user objected that a feature list means wrapping every feature
behind a clean cross-API boundary. Counter-proposal: D3D12 has no enable step, so Vulkan
enables everything supported (except robustness); native additions for overrides;
queries wrapped only for features RI's commands use. "Go" → P60.

## Review round B6 (2026-10-07)

**GPU addresses: an address query, with buffers always addressable?** "Go" → P61.

## Review round B7 (2026-10-07)

**Work-graph inputs and library root signatures?** "Ok, I am still apprehensive about
P45's markings, but let's go for now" → P62, with S4 opened to revisit P45.

## Review round B8 (2026-10-08)

**Present and submit.** First proposal: the next submit after `acquire()` waits on the
image, and a list touching the swapchain is valid for one frame only. The user suggested
hiding the screen and blitting to it internally on Vulkan. Then: "The real question is, do
we even want commandList resubmission, what does this enable? Today basically all the
command buffers are re-recorded each frame anyways. If we need it, how does D3D12 handle
the swapchain? For me we follow it, and only on Vulkan we add an emulated screen texture."
Answer: resubmission only saves CPU recording time, which matters little here, so drop it.
D3D12 hands out the current back buffer, and queue ordering does the rest. Without
resubmission, Vulkan can match this by hiding the semaphores, with no copy needed.
**Drop resubmission and follow D3D12 on both backends?** "Perfect, go" → P63 (amends P34).

## Review round B9 (2026-10-08)

**Swapchain format and color space: supported-pairs query, user picks, default helper,
tearing as a present mode, compute-to-screen as a capability?** "Ok, add a note about the
compute shader screen thing, this may be a good reason to emulate the screen with a normal
texture, anyways this needs to be clear to the user." → P64.

## Review round B10 (2026-10-08)

**Clears outside rendering: `clearBuffer/clearTexture(resource, slot, value)`, with a
helper for slot-free clears?** "No, no helper, we directly implement this behavior using
compute shader in DX12 (this is what most engines do anyways)" → P65. 

**B10a. Reserve a few slots at the end of the user heap for the D3D12 clear shaders, or
switch to an RI-owned heap during the clear (possible GPU stall)?** "For now let's go with
the potential stall, if it becomes a problem or we have other reasons to reserve part of
the heap, then we will reassess, note it down" → P65 (2).

## Review round B11 (2026-10-08)

**Memory budget query?** "Do we even need this?" Only for programs near the VRAM limit, and
the native query is a few lines. "Drop it, this is completely irrelevant for our use case.
If some user requests it we will add it" → P66.

## Review round B12 (2026-10-08)

**Reflection: D3D12 reads the root signature natively; Vulkan uses SPIRV-Reflect or an
in-house reader? dxcompiler at runtime only in validation builds?** "SPIRV-Reflect for now,
I agree with your validation-only proposal for dxcompiler" → P67. Review group B is done.

## Review round C1 (2026-10-08)

**Scope of RI's own layout switches on D3D12 / RDNA2 Vulkan: full scope (wait for all,
block all), measured with L1, or merge the user's barrier into the switch?** "Option 1"
→ P68.

## Review round C2 (2026-10-08)

**Discard: always required after aliasing (load ops don't count), full-scope barrier,
reported by validation?** "Ok" → P69.

## Review round C3 (2026-10-08)

**Native sections: `endNative` restores everything RI relies on (heap/set, layout, last
pipeline, root data); allowed inside rendering sections with limits?** "Ok" → P70.

## Review round C4 (2026-10-08)

**Adding queues later: narrow P37 (texture hand-off between queues is open), note it in
L1, add an optional measurement?** "Ok" → P37 amended, LayoutEvaluation item 6.

## Review round C5 (2026-10-08)

**Queries: explicit `resetQueries` (no-op on D3D12), a pipeline-statistics result-layout
query, timestamp frequency per queue once queues exist?** "Go" → P71.

## Review round C6 (2026-10-08)

**AS compaction: a compacted-size query type in the P51 pools?** "Ray tracing is not a main
target for v1" → R50 deferred until ray tracing is implemented; feature table updated.
Review group C is done.

## Open item Y4 (2026-10-08)

**Fine-grained barriers: no resource-naming barrier in RI; fine-grained = precise stages
plus native sections?** "Ok" → P72.

## Open item R10 (2026-10-08)

**Naming part of a texture: view objects created once (with a default whole-texture
view), or ranges written inline every time (hidden native view creation/cache)?** "1"
→ P73.

**Format reinterpretation: an optional list of extra view formats at texture creation,
empty by default?** "Go" → P74.

**Read-only depth: `depthReadOnly` / `stencilReadOnly` flags on the depth attachment?**
"Ok" → P75.

**Depth/stencil resolve modes with a capability query, no emulation?** "Why should it not
do sampleZero emulation?" I proposed emulating depth `sampleZero` on D3D12 (internal draw
writing `SV_Depth`, like P65) plus a general emulation rule. "No you are right, we don't
implement sample zero in D3D" → P76, no emulation and no general rule.

Noted while checking: native-to-RI import (review R30, part 2) was never discussed; added
to §5 as I1.

## Open item F1 (2026-10-09)

**Sparse / tiled resources: not in v1, added later as a queue operation plus a capability
query when a workload needs it?** "Ok" → feature table.

## Open item F2 (2026-10-10)

**Variable-rate shading: not in v1 (per-draw and per-primitive already work natively), the
rate image in `beginRendering` later?** "Ok" → feature table.

## Open item F3 (2026-10-10)

**Cooperative matrix / vectors: shader-only, enabled by P60, queried natively, so not an
RI concern?** "Go" → feature table.

## Open item I1 (2026-10-10)

**Native-to-RI import: import calls (RI does not own imported objects; textures in the
resting layout), plus raw native descriptor writes?** "Go" → P77.

## Open item X1 (2026-10-10)

**Status returns change signatures, so decide now which calls return "result or error"
(allocation, currentTexture/present, pipeline creation)?** "I don't care, changing every
user call is not a problem as I am the only user until this is resolved, I will test it on
myself until I am satisfied with the API" → X1 stays open by design; P57 notes updated.

## Open item L1, pre-measurement choices (2026-10-10)

**RDNA2 Vulkan: implement both modes (`GENERAL` everywhere / around-operation switches),
measure, pick automatically per GPU and driver, with a force option?** "Ok" → P78.

**Decision rule: frame ≤ 2% slower than optimal hand-written transitions, no pass > 10%
slower unless under 0.05 ms, on every tested GPU?** "Ok" → P79.

## Inference review (2026-10-10)

The user: "we infer nothing" (P36) is too harsh; soften to "we prefer not inferring, but if
it is evident or it simplifies the API greatly, we check"; review all decisions first,
don't change the P rows yet. Review in `docs/Review-Inference-2026-10-10.md`.

**"We check": (a) RI infers and does it, or (b) validation?** "a".

**Query reset (A2).** "What are the effects of query reset? What about host reset?" →
explained the Vulkan query states and GPU vs host reset. "What does query usage look like
exactly using RI?" → code sketch. "What does resolve queries do on each backend?" →
explained. "Ok let's go with CPU query reset." → A2 decided: host reset by the user after
reading; pool host-reset at creation.

**RI's own layout switches (A1): the half touching RI's own operation becomes exact, the
other half stays full?** "Ok" → A1 decided (entering: exact "after"; leaving: exact
"before", the mirror case, noted to the user).

**Work-graph INITIALIZE flag (A3): set automatically on first use or when the backing
memory changes, plus a "force initialize" flag?** "Ok" → A3 decided.

**D3D12 root signature without `[RootSignature]` (A4).** "Show me a sample shader with and
without" → samples. "Show me with the helper" → P45 helper sample. "Is there no way to get
sizeof? Do we really need EQ_ROOT_SIGNATURE, couldn't that just be at the end of root
data?" → tested with DXC: `_Static_assert(sizeof)` checks the count; `-rootsig-define`
removes the attribute. "Why is EQ_ROOT_COUNT out of EQ_ROOT_DATA? We don't need
rootsig-define, we can just do a big macro" → tested, works. "What to do with several
entry points? How does that even compile to SPIR-V? I don't understand your questions" →
explained (one compile per entry point; extra entry points need `EQ_ROOT_SIGNATURE`),
reduced to two choices. "We don't want to load dxcompiler at runtime because we don't want
to force the user to have HLSL available at runtime" → A4 decided: keep the markings with
the single big macro.

**Iterating on the macro.** "Ideally EQ_ROOT_SIGNATURE does NOT require the size to be put
again" → a count `#define` next to the struct. "No, this is not good enough, dig more" →
found the constant-buffer size in the DXIL `dx.resources` metadata (readable without
dxcompiler; dxil-spirv as prior art), so RI can build the root signature itself. "This is
good, but I would prefer a macro to mark a specific constant buffer as the root data" →
marking by reserved register space (space 999), tested. "Ideally I would not want a
reserved space, what are our other options?" → type-name marking (`EquineRootData`,
relies on a DXC detail) or "only constant buffer" rule. "There are no additional costs to
space 999?" → none beyond the reservation. "Let's go" → A4 final: reserved space.

**Apply the review to the P rows?** "Yes" → P36, P45, P62, P67, P68, P71 updated; S4
settled.

## Next steps (2026-10-10)

"Commit. Next" → committed (437b0ab); proposed an implementation plan. "Let's drop U1,
ideally the final EquineEngine API is similar to the current EquineEngine one, EquineRI on
the other hand, I don't really care, make proposals" → P80; proposals in
`docs/RIApiProposal.md`.

**RI object model: handles (A), thin value objects (B), or RAII (C)?** "B" → P81.

**Function placement, descriptions, backends, naming.** "Why is device an object? What
about backend swapping, doesn't this produce name collisions?" → device as object vs free
functions vs hidden global; per-backend namespaces with an `RI::` alias. "I really dislike
object oriented programming ... I want to be very sure that this Device object has good
reasons to live" → pros/cons; plain structs + free functions (P82). ".vk.? .d3d12.? Do we
share a global device struct? ... I use an unnamed struct with a field named internal" →
no shared struct, one set per backend namespace (P83), `internal` convention (P82). "I have
a problem with device.device, native access must be more gated" → native header + explicit
backend namespace. "Is it possible to have begin/endNative regions for each backend and it
compiles?" → `#if EQUINE_BACKEND_*` blocks. "I like the native header, this feels good" →
P84. Open: commands as free functions are not clearly "command list" at a glance.

**Commands: `cmd` prefix, `RI::Cmd::` namespace, or methods on the command list?** "How
would the methods work with the native include?" → they cannot be added from the native
header: mixed styles or the backend name disappears. "Go with 1, a layer on top can always
create an object with methods from it" → P85.

**Descriptions (designated initializers) and naming (V2 / vulkan.hpp conventions)?** "Yes"
→ P86. The RI API style is decided; sketches rewritten in `docs/RIApiProposal.md`.

The user edited `RIApiProposal.md` (removed "EquineEngine should look like the V2 API").
"How would overlapping frames look like? What is the use of a timeline point? How do we
pass surface to the backend? It is not clear what writeXBuffer is" → explained (frames-in-
flight loop with per-slot timeline points; timeline point = queue counter value; portable
`RI::Window` struct of OS handles; descriptor writes). "Yes change P80, do not pollute RI
with the subsequent layer I will build on top" → P80 amended. "For descriptor writes, in
Vulkan we try to batch them, what about here?" → batch form `writeDescriptors(device, heap,
writes)`. "Ok, keep this open until we are sure if we use the Vulkan extensions" → D1.

"storageTexture/sampledTexture etc. are a bit too verbose, give other options (they map to
what on each backend? On DX it is just SRV and UAV anyway). What about a validation
backend?" → Srv/Uav/Cbv, Read/ReadWrite/Constants, or helpers; a forwarding validation
backend. "Having View in the text clarifies ... but [ConstView] obscures what a CBV is. I
am not opposed to the helper free functions uav, srv, cbv, but the name needs to be good
for people that don't use those. I don't really care how we validate ... propose" →
ReadView/ReadWriteView/ConstantBufferView vs RView/RWView/CBView; validation: forwarding
backend, shared check functions, or scope reduction. "2+3 is good, A is good too" → P87,
P88.
