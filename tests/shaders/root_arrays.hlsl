// Root data with arrays: under DX constant-buffer packing every array element
// starts a new 16-byte register, so the size differs from the C-style sum.
#include <equine/root.hlsli>

struct ArrayRoot {
    uint slot;     // offset 0
    float w[4];    // offsets 16, 32, 48, 64
    uint2 pair[2]; // offsets 80, 96
    float tail;    // offset 104
};
EQ_ROOT_DATA(ArrayRoot, root);

[numthreads(16, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    RWStructuredBuffer<float> output = ResourceDescriptorHeap[root.slot];
    output[id.x] = root.w[id.x % 4] + float(root.pair[id.x % 2].y) + root.tail;
}
