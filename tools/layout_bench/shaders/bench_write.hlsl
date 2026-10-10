// Writes a pattern into a storage texture.
#include <equine/root.hlsli>

struct WriteRoot {
    uint textureSlot;
    uint width;
    uint height;
    uint seed;
};
EQ_ROOT_DATA(WriteRoot, root);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= root.width || id.y >= root.height) {
        return;
    }
    RWTexture2D<float4> t = ResourceDescriptorHeap[root.textureSlot];
    const uint h = (id.x * 73856093u) ^ (id.y * 19349663u) ^ root.seed;
    t[id.xy] = float4((h & 255) / 255.0f, ((h >> 8) & 255) / 255.0f, ((h >> 16) & 255) / 255.0f, 1);
}
