// Copies texture texels (one sample of MSAA textures) to a buffer of float4.
#include <equine/root.hlsli>

struct ReadRoot {
    uint textureSlot;
    uint outputSlot;
    uint width;
    uint height;
    uint multisampled; // 1: read sample `sampleIndex` of a Texture2DMS
    uint sampleIndex;
};
EQ_ROOT_DATA(ReadRoot, root);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= root.width || id.y >= root.height) {
        return;
    }
    RWStructuredBuffer<float4> output = ResourceDescriptorHeap[root.outputSlot];
    float4 value;
    if (root.multisampled != 0) {
        Texture2DMS<float4> texture = ResourceDescriptorHeap[root.textureSlot];
        value = texture.Load(int2(id.xy), root.sampleIndex);
    } else {
        Texture2D<float4> texture = ResourceDescriptorHeap[root.textureSlot];
        value = texture.Load(int3(id.xy, 0));
    }
    output[id.y * root.width + id.x] = value;
}
