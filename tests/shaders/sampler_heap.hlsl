// Sampler heap indexing only: the texture and the output are declared resources.
#include <equine/root.hlsli>

struct SamplerRoot {
    uint samplerSlot;
};
EQ_ROOT_DATA(SamplerRoot, root);

Texture2D tex : register(t0);
RWStructuredBuffer<float> output : register(u0);

[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    SamplerState s = SamplerDescriptorHeap[root.samplerSlot];
    output[id.x] = tex.SampleLevel(s, float2(0.5f, 0.5f), 0.0f).x;
}
