// Copies every texel of a 3D texture's first mip into the output buffer,
// slice after slice.
#include <equine/root.hlsli>

struct ReadVolumeRoot {
    uint textureSlot;
    uint outputSlot;
    uint size;
    uint depth;
};
EQ_ROOT_DATA(ReadVolumeRoot, root);

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID) {
    Texture3D<float4> volume = ResourceDescriptorHeap[root.textureSlot];
    RWStructuredBuffer<float4> output = ResourceDescriptorHeap[root.outputSlot];
    if (id.x < root.size && id.y < root.size && id.z < root.depth) {
        output[(id.z * root.size + id.y) * root.size + id.x] = volume.Load(int4(id, 0));
    }
}
