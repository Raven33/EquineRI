// Misuse (P112): slot 8 is past the EQ_CONSTANT_BUFFER slots.
#include <equine/root.hlsli>

struct Scale {
    float4 values;
};
EQ_CONSTANT_BUFFER(Scale, scale, 8);

[numthreads(1, 1, 1)]
void main() {
    RWStructuredBuffer<float> output = ResourceDescriptorHeap[0];
    output[0] = scale.values.x;
}
