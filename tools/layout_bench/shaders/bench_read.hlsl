// Reads every texel of a texture (sample 0 of MSAA ones) and writes one sum
// per 8x8 tile, so the reads cannot be optimized away.
#include <equine/root.hlsli>

struct ReadRoot {
    uint textureSlot;
    uint outputSlot;
    uint width;
    uint height;
    uint multisampled;
    uint pad[3];
};
EQ_ROOT_DATA(ReadRoot, root);

groupshared float tile[64];

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex, uint3 group : SV_GroupID) {
    float4 value = 0;
    if (id.x < root.width && id.y < root.height) {
        if (root.multisampled != 0) {
            Texture2DMS<float4> t = ResourceDescriptorHeap[root.textureSlot];
            value = t.Load(int2(id.xy), 0);
        } else {
            Texture2D<float4> t = ResourceDescriptorHeap[root.textureSlot];
            value = t.Load(int3(id.xy, 0));
        }
    }
    tile[index] = value.x + value.y + value.z + value.w;
    GroupMemoryBarrierWithGroupSync();
    if (index == 0) {
        float sum = 0;
        for (uint i = 0; i < 64; ++i) {
            sum += tile[i];
        }
        RWStructuredBuffer<float> output = ResourceDescriptorHeap[root.outputSlot];
        output[group.y * ((root.width + 7) / 8) + group.x] = sum;
    }
}
