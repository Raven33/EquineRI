// Point lists without a geometry shader: vertex v is a point at the center of
// pixel (v % 32, v / 32) of a 32 x 32 target (kSize in TextureTests.cpp). Root
// data as solid_ps.hlsl's, which every stage must share (P44).
#include <equine/root.hlsli>

struct DrawRoot {
    float4 color;
    float depth;
};
EQ_ROOT_DATA(DrawRoot, root);

static const uint kWidth = 32;

float4 main(uint v : SV_VertexID) : SV_Position {
    const float x = ((v % kWidth) + 0.5) / kWidth * 2 - 1;
    const float y = 1 - ((v / kWidth) + 0.5) / kWidth * 2;
    return float4(x, y, root.depth, 1);
}
