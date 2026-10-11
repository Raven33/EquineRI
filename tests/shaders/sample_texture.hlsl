// Samples a texture through `count` samplers of the sampler heap (P87), at
// four fixed coordinates; output[i] holds sampler i's red channel at each.
#include <equine/root.hlsli>

struct SampleRoot {
    uint textureSlot;
    uint outputSlot;
    uint count;
};
EQ_ROOT_DATA(SampleRoot, root);

[numthreads(1, 1, 1)]
void main() {
    Texture2D<float4> texture = ResourceDescriptorHeap[root.textureSlot];
    RWStructuredBuffer<float4> output = ResourceDescriptorHeap[root.outputSlot];
    for (uint i = 0; i < root.count; ++i) {
        SamplerState s = SamplerDescriptorHeap[i];
        output[i] = float4(texture.SampleLevel(s, float2(0.5, 0.5), 0).r, texture.SampleLevel(s, float2(0.0, 0.0), 0).r,
                           texture.SampleLevel(s, float2(0.75, 0.25), 0).r,
                           texture.SampleLevel(s, float2(-0.5, -0.5), 0).r);
    }
}
