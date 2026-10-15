# Threads

RI objects are not thread-safe. One thread, the **device thread**, drives a device: it is the
thread that called `createDevice`. A short list of calls may also run on other threads at the
same time ([P106](../design-notes/decisions.md#p106)).

## The any-thread list

These calls may run on any thread, at the same time as each other and as recording and
submission on the device thread:

| Calls | Why they are on the list |
|---|---|
| `createComputePipeline`, `createGraphicsPipeline`, `createWorkGraph` | Creating pipelines is the slow part of loading. On a cold driver, a work graph costs about half a second per node on one core |
| `workGraphEntry` | The thread that creates a graph usually wants its entry indices |
| `wait`, `isReached` | A readback or capture thread can wait for a timeline point while the device thread keeps recording |
| `bufferRequirements`, `textureRequirements`, `formatSupported`, `copyLayout`, `pipelineStatisticsLayout`, `blasSizes`, `tlasSizes`, `accelerationStructureRequirements` | Pure queries: loader threads can plan sizes and layouts |

A pipeline created on another thread can be named in its desc
([names at creation](../reference/debugging.md)), so the creating thread never needs
`setName`.

Everything else runs on the device thread. That includes recording and submission, creating and
destroying resources, writing descriptors, destroying pipelines, and `setName`.

```cpp
// Worker threads build pipelines while the device thread records frames.
std::vector<std::future<RI::Pipeline>> jobs;
for (const Kernel &kernel : kernels) {
    jobs.push_back(std::async(std::launch::async, [&device, &kernel] {
        RI::Helpers::ShaderCompiler compiler = RI::Helpers::createShaderCompiler(); // one per thread
        const RI::Helpers::CompiledShader shader = RI::Helpers::compileShader(compiler, kernel.source);
        RI::Helpers::destroyShaderCompiler(compiler);
        return RI::createComputePipeline(device, {.shader = shader.bytecode, .name = kernel.name});
    }));
}
```

Helpers follow the same rule. Use one [`ShaderCompiler`](../helpers/shader-compiler.md) per
thread; several may exist at once. The allocators, transfers, frame ring and profiler run on the
device thread.

## What is checked

In validation builds, every call outside the list checks that it runs on the device thread. A
call from another thread is a **programmer error** that names the call and both threads:

```text
[EquineRI] programmer error: createCommandList: called from thread 140213, but the device thread
is 140207 (the thread that called createDevice). Only the any-thread list (...) may run on
other threads (P106)
```

Release builds check nothing and pay nothing.

## What stays out, for now

- **Recording or submitting on several threads.** Research frames record few commands, and
  RI fills some caches while recording.
- **Creating or destroying resources on a loader thread.** Texture creation submits RI's own
  internal commands on the queue, and Vulkan descriptor writes need a lock per heap.
- **Handing the device thread to another thread.** A device created on one thread and driven
  from another would need a hand-over call; none exists yet.

## Related decisions

- [P106](../design-notes/decisions.md#p106): the any-thread list and the device-thread check.
- [P56](../design-notes/decisions.md#p56): one recording thread; command lists reused only after
  their point.
- [P34](../design-notes/decisions.md#p34): immediate recording.
