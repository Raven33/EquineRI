---
symbols: [RI::writeDescriptors, RI::DescriptorWrite, RI::Descriptor, RI::ReadView, RI::ReadWriteView, RI::ConstantBufferView, RI::kWholeSize]
---

# RI::writeDescriptors

## What it does

`RI::writeDescriptors(device, heap, writes)` writes a list of descriptors into heap slots. Each
`RI::DescriptorWrite` names a **slot** and a **descriptor**: one of three views of a buffer
([P87](../design-notes/decisions.md#p87)).

| View | Shader type | Buffer usage needed |
|---|---|---|
| `RI::ReadView` | `StructuredBuffer<T>`, `ByteAddressBuffer` | `eRead` |
| `RI::ReadWriteView` | `RWStructuredBuffer<T>`, `RWByteAddressBuffer` | `eReadWrite` |
| `RI::ConstantBufferView` | `ConstantBuffer<T>` | `eConstant` |

Each view covers part of a buffer: `offset` (bytes) and `size` (bytes; `RI::kWholeSize`, the
default, means "to the end"). Read and read-write views also have a `stride`: the element size
of a structured buffer, or **0 for a raw** (byte address) buffer.

The writes **take effect at once**: there is no "commit" step. You pass a braced list, or a
span (for example a `std::vector`) built at run time; one write is a list of one
([P94](../design-notes/decisions.md#p94), D1).

## Signature

--8<-- "generated/signatures/RI.writeDescriptors.md"

--8<-- "generated/signatures/RI.DescriptorWrite.md"

--8<-- "generated/signatures/RI.Descriptor.md"

--8<-- "generated/signatures/RI.ReadView.md"

--8<-- "generated/signatures/RI.ReadWriteView.md"

--8<-- "generated/signatures/RI.ConstantBufferView.md"

--8<-- "generated/signatures/RI.kWholeSize.md"

## Example

```cpp
--8<-- "snippets/ri/compute.cpp:write-descriptors"
```

A list built at run time, with a view of part of a buffer:

```cpp
--8<-- "snippets/ri/compute.cpp:write-descriptors-span"
```

In the shader, the slot comes from root data:

```hlsl
--8<-- "snippets/hlsl/doc_compute.hlsl:heap-access"
```

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/compute.cpp:write-descriptors"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/compute.cpp:write-descriptors"
    ```

    What to watch for: in SPIR-V, read-only and read-write structured buffers are both
    *storage buffers*; constant buffers are *uniform buffers*. Into a mutable descriptor,
    each write states the concrete type. RI writes the whole list with **one**
    `vkUpdateDescriptorSets` call.

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:write-descriptors"
    ```

    What to watch for: buffer views count **elements**, not bytes: `FirstElement` and
    `NumElements` in units of `StructureByteStride`, or of 4 bytes for raw views
    (`R32_TYPELESS` with the `RAW` flag). Constant buffer views are addressed by GPU address
    and sized in 256-byte blocks. RI makes one call per write.

## Rules and errors

All checked in validation builds (**programmer error**):

- the **slot** is inside the heap: `slot 8 is outside the resource heap (it has 8 slots)`;
- **read and read-write views** start at a multiple of 16 bytes and of the element size
  (`stride`, or 4 for raw views), are not empty, and cover whole elements;
- **constant buffer views** start at a multiple of 256 bytes. Their size is **rounded up to
  256** on both backends, and the rounded size must fit in the buffer: give constant buffers a
  size that is a multiple of 256;
- the buffer is alive (not destroyed).

Also:

- Never overwrite a slot that GPU work still in flight may read; wait for its
  [timeline point](../concepts/timeline.md) ([P59](../design-notes/decisions.md#p59)).
- Only the views heap is written today; samplers come with textures.

## Related decisions

- [P87](../design-notes/decisions.md#p87): descriptor kinds named by access.
- [P42](../design-notes/decisions.md#p42): you choose the slots.
- [P59](../design-notes/decisions.md#p59): writes take effect at once; never overwrite a slot
  in use.
- [P94](../design-notes/decisions.md#p94): the batching form (D1) and the view alignment
  rule.
