// Bindful constant buffers (P112): slots 0 and 3, the second named by a macro,
// beside root data. output[root.base + i] = i * scale.x + offsets.a.y + offsets.b.z.
#include <equine/root.hlsli>

struct ConstantRoot {
    uint outputSlot;
    uint base;
};
EQ_ROOT_DATA(ConstantRoot, root);

struct Scale {
    float4 values;
};
struct Offsets {
    float4 a;
    float4 b;
};
#define OFFSETS_SLOT 3
EQ_CONSTANT_BUFFER(Scale, scale, 0);
EQ_CONSTANT_BUFFER(Offsets, offsets, OFFSETS_SLOT);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    RWStructuredBuffer<float> output = ResourceDescriptorHeap[root.outputSlot];
    output[root.base + id.x] = float(id.x) * scale.values.x + offsets.a.y + offsets.b.z;
}
