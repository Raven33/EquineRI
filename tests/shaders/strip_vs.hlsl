// Four vertices of a triangle strip covering the target, in root.color.
#include <equine/root.hlsli>

struct DrawRoot {
    float4 color;
    float depth;
};
EQ_ROOT_DATA(DrawRoot, root);

struct VertexOut {
    float4 position : SV_Position;
    float4 color : COLOR0;
};

VertexOut main(uint vertex : SV_VertexID) {
    VertexOut o;
    o.position = float4((vertex & 1) ? 1.0 : -1.0, (vertex & 2) ? -1.0 : 1.0, root.depth, 1);
    o.color = root.color;
    return o;
}
