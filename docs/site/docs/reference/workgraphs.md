---
symbols: [RI::createWorkGraph, RI::destroyWorkGraph, RI::workGraphEntry, RI::cmdSetWorkGraph, RI::cmdDispatchGraph, RI::cmdDispatchGraphIndirect, RI::WorkGraph, RI::WorkGraphDesc, RI::WorkGraphMemory, RI::GraphInput]
---

# Work graphs

## What they are

A **work graph** is a set of compute shaders (nodes) that launch each other on the GPU: a
node writes records, and the GPU schedules the nodes that consume them, with no CPU round trip
([P50](../design-notes/decisions.md#p50), [P62](../design-notes/decisions.md#p62)).

**D3D12 only**, and only where `device.limits.workGraphs` is set (Ampere or RDNA3 and newer,
D3D12 work graphs tier 1.0). On Vulkan every call is **unsupported**
(`VK_AMDX_shader_enqueue` is provisional and AMD-only).

- Write the nodes in one HLSL library and compile it with profile `lib_6_8` (RI's shader rule
  makes DXIL only for `lib_` profiles).
- `RI::createWorkGraph(device, {library, rootDataSize})` creates the graph with every node of
  the library. RI does not read root data from libraries ([P62](../design-notes/decisions.md#p62)):
  give the size of the `EQ_ROOT_DATA` block the nodes declare. `graph.backing` gives the
  backing memory it needs (`minSize`, `maxSize`).
- `RI::workGraphEntry(graph, "nodeName")` gives an entry node's index.
- `RI::cmdSetWorkGraph(cmd, graph, backingBuffer, offset, size)` sets the graph and its backing
  memory (a range of a buffer with `eReadWrite`); `cmdSetRootData` then feeds its nodes.
- `RI::cmdDispatchGraph(cmd, inputs)` launches it with CPU records (copied into the command
  list): one `GraphInput` for one entry node, several for several.
  `RI::cmdDispatchGraphIndirect(cmd, address, multiNode)` launches it with GPU-written input
  in D3D12's `D3D12_NODE_GPU_INPUT` / `D3D12_MULTI_NODE_GPU_INPUT` layout.

**Backing memory initialization is inferred** ([P62](../design-notes/decisions.md#p62)): RI
initializes the backing memory on a graph's first use and whenever its range changes; pass
`forceInitialize` to reset it deliberately.

## Signature

--8<-- "generated/signatures/RI.createWorkGraph.md"

--8<-- "generated/signatures/RI.cmdSetWorkGraph.md"

--8<-- "generated/signatures/RI.cmdDispatchGraph.md"

## Example

```hlsl
[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(64, 1, 1)]
void entry(DispatchNodeInputRecord<EntryRecord> input, uint thread : SV_GroupThreadID,
           [MaxRecords(64)] NodeOutput<WorkRecord> work) { /* ... */ }
```

```cpp
RI::WorkGraph graph = RI::createWorkGraph(device, {.library = library, .rootDataSize = sizeof(GraphRoot)});
RI::cmdSetHeap(cmd, heap);
RI::cmdSetWorkGraph(cmd, graph, backing, 0, graph.backing.minSize);
RI::cmdSetRootData(cmd, GraphRoot{.outputSlot = 0});
const RI::GraphInput input{.entry = RI::workGraphEntry(graph, "entry"), .records = &record, .recordCount = 1,
                           .recordStride = sizeof(record)};
RI::cmdDispatchGraph(cmd, {&input, 1});
```

## How RI does it

- **D3D12**: a state object with the DXIL library, a global root signature built by RI (root
  constants and directly indexed heaps, as for every RI pipeline), and a work graph including
  all nodes; `SetProgram` with the backing memory (and the `INITIALIZE` flag when inferred),
  then `DispatchGraph`.

## Rules and errors

- Set a descriptor heap before the graph, as before a pipeline.
- The backing range has at least `graph.backing.minSize` bytes of an `eReadWrite` buffer
  (**programmer error** otherwise).
- After `cmdSetWorkGraph`, set a pipeline again before ordinary dispatches.
- Node-local root arguments and graphs with several programs are not modelled yet; use
  [native access](../native/index.md).

## Related decisions

- [P50](../design-notes/decisions.md#p50): work graphs, D3D12 only.
- [P62](../design-notes/decisions.md#p62): input modes, inferred initialization, root data.
