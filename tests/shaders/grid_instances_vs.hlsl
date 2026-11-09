// A point per vertex and instance, for geometry shaders: vertex v of instance
// i is the center of pixel ((v + 16 * i) % 32, (v + 16 * i) / 32) of a 32 x 32
// target, with root.color in COLOR0. Root data as solid_ps.hlsl's (P44).
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

static const uint kWidth = 32;

VertexOut main(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
    const uint v = vertex + 16 * instance;
    VertexOut o;
    o.position = float4(((v % kWidth) + 0.5) / kWidth * 2 - 1, 1 - ((v / kWidth) + 0.5) / kWidth * 2, root.depth, 1);
    o.color = root.color;
    return o;
}
