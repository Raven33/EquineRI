// Full-screen triangle for the post pass (root data shared with the pixel shader).
#include <equine/root.hlsli>

struct PostRoot {
    uint litSlot;
    uint pad0;
    uint pad1;
    uint pad2;
};
EQ_ROOT_DATA(PostRoot, root);

float4 main(uint vertex : SV_VertexID) : SV_Position {
    const float2 uv = float2((vertex << 1) & 2, vertex & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
