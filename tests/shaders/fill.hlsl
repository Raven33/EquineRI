// Buffer -> compute -> readback (Milestone 2 exit): out[i] = in[i] * scale + offset.
#include <equine/root.hlsli>

struct FillRoot {
    uint inputSlot;  // ReadView of the input buffer
    uint outputSlot; // ReadWriteView of the output buffer
    uint paramsSlot; // ConstantBufferView holding `offset`
    float scale;
    uint count;
};
EQ_ROOT_DATA(FillRoot, root);

struct Params {
    float offset;
};

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= root.count) {
        return;
    }
    StructuredBuffer<float> input = ResourceDescriptorHeap[root.inputSlot];
    RWStructuredBuffer<float> output = ResourceDescriptorHeap[root.outputSlot];
    ConstantBuffer<Params> params = ResourceDescriptorHeap[root.paramsSlot];
    output[id.x] = input[id.x] * root.scale + params.offset;
}
