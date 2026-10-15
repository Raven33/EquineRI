// A triangle in the middle of the target, for wireframe tests.
#include <equine/root.hlsli>

struct DrawRoot {
    float4 color;
    float depth;
};
EQ_ROOT_DATA(DrawRoot, root);

float4 main(uint vertex : SV_VertexID) : SV_Position {
    const float2 points[3] = {float2(-0.7, -0.7), float2(0.7, -0.7), float2(0.0, 0.7)};
    return float4(points[vertex], root.depth, 1);
}
