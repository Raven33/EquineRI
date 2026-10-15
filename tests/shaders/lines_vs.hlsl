// Three points for line tests: across the middle row, then up the right side.
#include <equine/root.hlsli>

struct DrawRoot {
    float4 color;
    float depth;
};
EQ_ROOT_DATA(DrawRoot, root);

float4 main(uint vertex : SV_VertexID) : SV_Position {
    const float2 points[3] = {float2(-0.9, 0.03), float2(0.9, 0.03), float2(0.9, 0.9)};
    return float4(points[vertex], root.depth, 1);
}
