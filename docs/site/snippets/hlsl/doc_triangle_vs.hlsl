// The vertex shader of the documentation's rendering examples: one triangle
// covering the whole target, with no vertex buffer (vertex pulling, P39).
// Compiled to SPIR-V and DXIL by EQUINE_BUILD_DOC_SNIPPETS (profile vs_6_6).

// --8<-- [start:main]
#include <equine/root.hlsli>

struct DrawRoot {
    float4 color; // read by the pixel shader
    float depth;  // the triangle's depth
};
EQ_ROOT_DATA(DrawRoot, root);

// Vertices 0, 1, 2 give (-1, 1), (3, 1), (-1, -3) in clip space: a triangle
// that covers the target. Clip-space y points up on both backends.
float4 main(uint vertex : SV_VertexID) : SV_Position {
    const float2 uv = float2((vertex << 1) & 2, vertex & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), root.depth, 1);
}
// --8<-- [end:main]
