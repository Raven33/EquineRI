// Full-screen pass reading `lit` into the output target.
#include <equine/root.hlsli>

struct PostRoot {
    uint litSlot;
    uint pad0;
    uint pad1;
    uint pad2;
};
EQ_ROOT_DATA(PostRoot, root);

float4 main(float4 position : SV_Position) : SV_Target0 {
    Texture2D<float4> lit = ResourceDescriptorHeap[root.litSlot];
    const float3 c = lit.Load(int3(position.xy, 0)).rgb;
    return float4(c / (1.0f + c), 1);
}
