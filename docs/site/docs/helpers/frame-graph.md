# Frame graph

!!! note "First shape"
    This is the first shape of the frame graph ([P35](../design-notes/decisions.md#p35)). It is open to review and may
    change.

## What it does

`<RI/Helpers/FrameGraph.hpp>` plans a frame from **declarations only**: you declare each
step's resource uses, compile once, then record the commands yourself. There are no pass
objects and no callbacks.

1. **Declare resources.** `addTransientTexture(desc)` is a texture the graph creates; its
   contents last one frame. `addExternal()` is something you own (a buffer, a persistent
   texture, the screen texture), tracked for barriers only.
2. **Declare steps**, in recording order: `addStep({{resource, Stage, Access}, ...})`.
   `Access::eWrite` also covers read-modify-write.
3. **Compile** (`compileFrameGraph`, once). The graph places the transient textures in one
   memory block, **aliasing** those whose lifetimes do not overlap, creates them, and plans
   one coarse barrier per step. The barrier covers every hazard with the steps before it,
   including the previous frame's steps, since frames repeat. It also plans a discard before
   each transient texture's first use in a frame ([P69](../design-notes/decisions.md#p69)). A transient texture read
   before it is written is a programmer error.
4. **Each frame**: `beginStep(graph, cmd, step)` records the step's discards and barrier;
   then record the step's commands. `graphTexture(graph, resource)` gives the texture.

The barriers are `cmdBarrier(before, after)` with stage masks: RI tracks no state
([P36](../design-notes/decisions.md#p36)), and the graph only adds what its declarations imply.

## Example

```cpp
--8<-- "snippets/ri/helpers.cpp:frame-graph"
```

## How to roll your own

The graph is a planner over three RI calls: `textureRequirements` and `createTexture` at
chosen offsets of one `Memory` (aliasing), `cmdDiscard` at first use, and `cmdBarrier`
with the union of the stages of earlier conflicting uses (before) and the step's own (after).
