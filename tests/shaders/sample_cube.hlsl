// Samples a cube texture along the six axes (+X, -X, +Y, -Y, +Z, -Z) into the
// output buffer, one float4 per face.
#include <equine/root.hlsli>

struct SampleCubeRoot {
    uint cubeSlot;
    uint samplerSlot;
    uint outputSlot;
};
EQ_ROOT_DATA(SampleCubeRoot, root);

[numthreads(6, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const float3 directions[6] = {float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0),
                                  float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1)};
    TextureCube<float4> cube = ResourceDescriptorHeap[root.cubeSlot];
    SamplerState nearest = SamplerDescriptorHeap[root.samplerSlot];
    RWStructuredBuffer<float4> output = ResourceDescriptorHeap[root.outputSlot];
    output[id.x] = cube.SampleLevel(nearest, directions[id.x], 0);
}
