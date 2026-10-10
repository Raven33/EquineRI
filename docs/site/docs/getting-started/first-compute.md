# Your first compute dispatch

This page walks through the sample `samples/compute_readback`, step by step. It uploads 4096
numbers to the GPU, runs a compute shader that computes `out[i] = in[i] * 3 + 0.25`, and
reads the result back on the CPU. It uses every part of RI that exists today: memory,
buffers, descriptor heaps, a compute pipeline, a command list, barriers and submission.

The same source runs on Vulkan and on D3D12: the build compiles it once per backend
([P83](../design-notes/decisions.md#p83)). It has been checked on an RTX 3080 on both
backends (Windows) and on an AMD Radeon 680M with Vulkan (Linux, RADV).

The code below comes from `docs/site/snippets/ri/compute.cpp`, which follows the sample
line for line and is compiled with the documentation snippets.

## Build and run it

The sample is built by every preset that builds samples (all of them):

=== "Linux"

    ```text
    cmake --preset clang-debug
    cmake --build --preset clang-debug
    ./build/clang-debug/samples/compute_readback_Vulkan
    ```

=== "Windows"

    ```text
    cmake --preset clangcl-debug
    cmake --build --preset clangcl-debug
    build\clangcl-debug\samples\compute_readback_Vulkan.exe
    build\clangcl-debug\samples\compute_readback_D3D12.exe
    ```

It prints the GPU, the first four results, and how many are wrong:

```text
GPU: <your GPU>
out[0..3] = 0.25 3.25 6.25 9.25; 0 of 4096 values wrong
```

`ctest` also runs it (as `sample.compute_readback.Vulkan` / `.D3D12`, labelled `gpu`).

!!! note "Leak reports on Linux"
    The debug presets build with AddressSanitizer. Some Vulkan drivers leak a few bytes of
    their own at exit, which the sanitizer reports when you run the sample by hand. `ctest`
    passes a suppression file for those; to do the same by hand, set
    `LSAN_OPTIONS=suppressions=<repository>/tests/vulkan/lsan.supp`.

## The shader

The shader is HLSL. Building the sample compiles it twice with the pinned DXC: to SPIR-V for
Vulkan and to DXIL for D3D12 (see [Compiling shaders](../concepts/shaders.md)).

```hlsl
--8<-- "snippets/hlsl/doc_compute.hlsl:root-data"
```

`EQ_ROOT_DATA` declares the shader's **root data**: a small struct of values the C++ side sets
for each dispatch ([Root data](../concepts/root-data.md)). Here it carries the heap slots of
the three buffers, the scale, and the element count.

```hlsl
--8<-- "snippets/hlsl/doc_compute.hlsl:main"
```

The shader does not declare its buffers. It fetches them from the **descriptor heap** by
slot number: that is bindless ([Descriptor heaps](../concepts/descriptors.md)).

## Step 1: a device

```cpp
--8<-- "snippets/ri/device.cpp:create-device"
```

See [`RI::listGpus`](../reference/listGpus.md) and
[`RI::createDevice`](../reference/createDevice.md).

## Step 2: memory, and buffers placed in it

In RI you allocate **memory blocks** yourself and place buffers in them at offsets you
choose ([Memory](../concepts/memory.md)). First ask how much room a buffer needs:

```cpp
--8<-- "snippets/ri/compute.cpp:requirements"
```

Then allocate three blocks of different **kinds**: GPU memory for the data the shader works
on, upload memory the CPU writes, readback memory the CPU reads.

```cpp
--8<-- "snippets/ri/compute.cpp:allocate-memory"
```

And place five buffers in them. Each usage flag says what the buffer will be used for:

```cpp
--8<-- "snippets/ri/compute.cpp:create-buffers"
```

## Step 3: write the input

Upload and readback memory stay **mapped** for their whole life: `buffer.mapped` is a CPU
pointer you write through directly.

```cpp
--8<-- "snippets/ri/compute.cpp:write-mapped"
```

The shader cannot read upload memory as fast as GPU memory, so the program will copy the
values into the `input` buffer on the GPU first.

## Step 4: descriptors

A **descriptor** tells the GPU where a buffer is and how the shader sees it. You write them
into a **descriptor heap**, at slots you choose:

```cpp
--8<-- "snippets/ri/compute.cpp:create-heap"
```

```cpp
--8<-- "snippets/ri/compute.cpp:write-descriptors"
```

`ReadView` matches `StructuredBuffer` in the shader, `ReadWriteView` matches
`RWStructuredBuffer`, and `ConstantBufferView` matches `ConstantBuffer`. See
[`RI::writeDescriptors`](../reference/writeDescriptors.md).

## Step 5: the pipeline

A **pipeline** is the compiled shader, ready to run. RI reads from the shader how big its root
data is; you give it only the bytecode.

```cpp
--8<-- "snippets/ri/compute.cpp:load-shader"
```

```cpp
--8<-- "snippets/ri/compute.cpp:create-pipeline"
```

## Step 6: record the commands

The GPU runs **command lists** that you record on the CPU. Recording does nothing on the GPU
yet; it only writes the commands down.

```cpp
--8<-- "snippets/ri/compute.cpp:create-command-list"
```

The C++ copy of the root data, with the same members in the same order as the shader:

```cpp
--8<-- "snippets/ri/compute.cpp:root-struct"
```

```cpp
--8<-- "snippets/ri/compute.cpp:record"
```

Line by line:

1. `cmdBegin` opens the list for recording.
2. `cmdCopyBuffer` copies the input from upload memory to GPU memory.
3. `cmdBarrier(eCopy, eCompute)`: the GPU runs commands in parallel unless told otherwise.
   This **barrier** makes the shader wait until the copy has finished writing
   ([Barriers](../concepts/barriers.md)).
4. `cmdSetHeap`, `cmdSetPipeline`, `cmdSetRootData`: what the dispatch will use. The heap
   must be set **before** the pipeline.
5. `cmdDispatch` runs the shader: 64 groups of 64 threads, one thread per number.
6. A second barrier makes the copy wait for the shader's writes, then the result is copied to
   readback memory.
7. `cmdEnd` closes the list.

## Step 7: submit, wait, read

```cpp
--8<-- "snippets/ri/compute.cpp:submit-wait"
```

`submit` hands the list to the GPU and returns at once with a **timeline point**: a number
the GPU reaches when it has finished this work. `wait` blocks the CPU until then
([Timeline points](../concepts/timeline.md)). After that, the readback memory holds the
result:

```cpp
--8<-- "snippets/ri/compute.cpp:read-back"
```

## Step 8: clean up

RI destroys nothing for you: every object is destroyed explicitly, and only when the GPU no
longer uses it ([P20](../design-notes/decisions.md#p20)).

```cpp
--8<-- "snippets/ri/compute.cpp:destroy"
```

## The same program in Vulkan and D3D12

Written with the native APIs alone, the same program is several hundred lines. The full
files are `docs/site/snippets/vulkan/compute.cpp` and `docs/site/snippets/d3d12/compute.cpp`;
each reference page shows the matching part. For example, recording:

=== "RI"

    ```cpp
    --8<-- "snippets/ri/compute.cpp:record"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/compute.cpp:record"
    ```

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:record"
    ```

## Where to go next

- [Memory kinds and placed resources](../concepts/memory.md)
- [Descriptor heaps and bindless](../concepts/descriptors.md)
- [Root data](../concepts/root-data.md)
- [Timeline points and command-list reuse](../concepts/timeline.md)
- [Barriers](../concepts/barriers.md)
- [Compiling shaders for RI](../concepts/shaders.md)
