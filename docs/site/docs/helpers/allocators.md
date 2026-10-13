# Memory and slot allocators

## What it does

RI places every buffer and texture at an offset you choose in a `Memory` you allocated
([P31](../design-notes/decisions.md#p31)), and you pick every descriptor slot ([P42](../design-notes/decisions.md#p42)). Two helpers do the
bookkeeping for you:

- **Memory allocator** (`<RI/Helpers/MemoryAllocator.hpp>`): keeps blocks of each
  `MemoryKind` (64 MiB by default; a larger request gets a block of its own) and places
  resources in them first fit. `allocate` takes `MemoryRequirements`; `createBuffer` and
  `createTexture` ask for the requirements, allocate and create in one call.
- **Slot allocator** (`<RI/Helpers/SlotAllocator.hpp>`): hands out the heap slots
  `first` .. `first + count - 1`. The rest of the heap stays yours.

Both free **late**: `release` and `releaseSlot` take the `TimelinePoint` of the last
submission that may use the range or slot, and reuse it only once that point is reached
([P20](../design-notes/decisions.md#p20), [P59](../design-notes/decisions.md#p59)). The memory allocator's default point means "free at once".

Blocks use each kind's usual memory. A resource whose `MemoryRequirements` rule that memory out
(Vulkan drivers may allow some resources only in some native types, such as indirect scratch
on RADV) needs its own block: `RI::allocateMemory(device, kind, size, &requirements)`.

When an allocation reuses a range that held another resource, `Allocation::needsDiscard` is
set: the new texture's contents are undefined, so `cmdDiscard` it before its first use
([P69](../design-notes/decisions.md#p69)).

The memory allocator keeps a pointer to the `Device` it was created with: keep the device at
the same address, and alive, until `destroyMemoryAllocator`.

## Example

```cpp
--8<-- "snippets/ri/helpers.cpp:allocator"
```

```cpp
--8<-- "snippets/ri/helpers.cpp:slots"
```

```cpp
--8<-- "snippets/ri/helpers.cpp:release"
```

## How to roll your own

- Memory: `allocateMemory` once per block, `bufferRequirements` / `textureRequirements` for
  each resource's size and alignment, then `createBuffer` / `createTexture` at the offset you
  chose. Keep a free list per block; keep released ranges aside until `isReached(queue, point)`.
- Slots: any free list of integers, with the same deferred reuse.
