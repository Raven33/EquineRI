// The control points of one patch: the corners of the full-screen triangle.
#include <equine/root.hlsli>

struct DrawRoot {
    float4 color;
    float depth;
};
EQ_ROOT_DATA(DrawRoot, root);

struct ControlPoint {
    float4 position : POSITION0;
};

ControlPoint main(uint vertex : SV_VertexID) {
    const float2 uv = float2((vertex << 1) & 2, vertex & 2);
    ControlPoint o;
    o.position = float4(uv * float2(2, -2) + float2(-1, 1), root.depth, 1);
    return o;
}
