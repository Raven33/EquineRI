// Writes the depth texture read at this pixel to red: sampling a read-only
// depth attachment inside its own section (P75). Root data as
// fullscreen_vs.hlsl's (P44: one size for every stage); the texture's heap
// slot is the bits of root.color.x.
#include <equine/root.hlsli>

struct DrawRoot {
    float4 color;
    float depth;
};
EQ_ROOT_DATA(DrawRoot, root);

float4 main(float4 position : SV_Position) : SV_Target0 {
    Texture2D<float> depth = ResourceDescriptorHeap[asuint(root.color.x)];
    return float4(depth.Load(int3(position.xy, 0)), 0, 0, 1);
}
