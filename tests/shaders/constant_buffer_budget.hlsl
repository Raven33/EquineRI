// Misuse (P112): 256 bytes of root data leave no room for a constant buffer.
#include <equine/root.hlsli>

struct FullRoot {
    uint4 values[16];
};
EQ_ROOT_DATA(FullRoot, root);

struct Scale {
    float4 values;
};
EQ_CONSTANT_BUFFER(Scale, scale, 0);

[numthreads(1, 1, 1)]
void main() {
    RWStructuredBuffer<float> output = ResourceDescriptorHeap[root.values[0].x];
    output[0] = scale.values.x;
}
