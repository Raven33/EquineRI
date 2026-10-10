// out[i] = in[i] * scale + offset, reading `offset` from a constant buffer.
#include <equine/root.hlsli>

struct Root {
    uint inputSlot;
    uint outputSlot;
    uint paramsSlot;
    float scale;
    uint count;
};
EQ_ROOT_DATA(Root, root);

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
