// A triangle covering the screen; its depth comes from constant buffer slot 1,
// which the pixel shader shares (P112).
#include <equine/root.hlsli>

struct Shape {
    float4 color;
    float depth;
};
EQ_CONSTANT_BUFFER(Shape, shape, 1);

float4 main(uint vertex : SV_VertexID) : SV_Position {
    const float2 uv = float2((vertex << 1) & 2, vertex & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), shape.depth, 1);
}
