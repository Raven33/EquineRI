// Writes (x, y, value, 1) into a storage texture.
#include <equine/root.hlsli>

struct WriteRoot {
    uint textureSlot;
    uint width;
    uint height;
    float value;
};
EQ_ROOT_DATA(WriteRoot, root);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= root.width || id.y >= root.height) {
        return;
    }
    RWTexture2D<float4> texture = ResourceDescriptorHeap[root.textureSlot];
    texture[id.xy] = float4(id.x, id.y, root.value, 1);
}
