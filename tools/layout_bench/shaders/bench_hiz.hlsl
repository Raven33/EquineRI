// The first level of a depth pyramid: max of each 2x2 block.
#include <equine/root.hlsli>

struct HizRoot {
    uint depthSlot;
    uint outputSlot;
    uint width; // of the output
    uint height;
};
EQ_ROOT_DATA(HizRoot, root);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= root.width || id.y >= root.height) {
        return;
    }
    Texture2D<float4> depth = ResourceDescriptorHeap[root.depthSlot];
    RWTexture2D<float> output = ResourceDescriptorHeap[root.outputSlot];
    const int2 p = int2(id.xy) * 2;
    const float a = depth.Load(int3(p, 0)).x;
    const float b = depth.Load(int3(p + int2(1, 0), 0)).x;
    const float c = depth.Load(int3(p + int2(0, 1), 0)).x;
    const float d = depth.Load(int3(p + int2(1, 1), 0)).x;
    output[id.xy] = max(max(a, b), max(c, d));
}
