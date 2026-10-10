// The compute shader of the documentation examples (the same as
// samples/compute_readback): out[i] = in[i] * scale + offset, with `offset`
// read from a constant buffer. Compiled to SPIR-V and DXIL by
// EQUINE_BUILD_DOC_SNIPPETS with RI's rule (equine_add_shaders, P93).

// --8<-- [start:root-data]
#include <equine/root.hlsli>

// Root data: a few values set per dispatch with cmdSetRootData (P45).
// The C++ side declares the same struct.
struct Root {
    uint inputSlot;  // descriptor heap slots of the three buffers
    uint outputSlot;
    uint paramsSlot;
    float scale;
    uint count;
};
EQ_ROOT_DATA(Root, root);
// --8<-- [end:root-data]

struct Params {
    float offset;
};

// --8<-- [start:main]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= root.count) {
        return;
    }
    // --8<-- [start:heap-access]
    // Bindless: fetch each buffer from the heap by the slot passed in root data.
    StructuredBuffer<float> input = ResourceDescriptorHeap[root.inputSlot];
    RWStructuredBuffer<float> output = ResourceDescriptorHeap[root.outputSlot];
    ConstantBuffer<Params> params = ResourceDescriptorHeap[root.paramsSlot];
    // --8<-- [end:heap-access]
    output[id.x] = input[id.x] * root.scale + params.offset;
}
// --8<-- [end:main]
