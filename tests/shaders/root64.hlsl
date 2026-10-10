// Root data of 64 bytes: one float4x4.
#include <equine/root.hlsli>

struct MatrixRoot {
    float4x4 transform;
};
EQ_ROOT_DATA(MatrixRoot, root);

[numthreads(32, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    RWStructuredBuffer<float4> output = ResourceDescriptorHeap[0];
    output[id.x] = mul(root.transform, float4(id, 1.0f));
}
