// Writes face f of a cube texture, seen as a 2D array, with the color
// (f / 8 + 1 / 16, 1 - f / 8, 0, 1).
#include <equine/root.hlsli>

struct WriteFacesRoot {
    uint textureSlot;
    uint size;
};
EQ_ROOT_DATA(WriteFacesRoot, root);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    RWTexture2DArray<float4> faces = ResourceDescriptorHeap[root.textureSlot];
    if (id.x < root.size && id.y < root.size) {
        faces[id] = float4(id.z / 8.0 + 1.0 / 16.0, 1.0 - id.z / 8.0, 0, 1);
    }
}
