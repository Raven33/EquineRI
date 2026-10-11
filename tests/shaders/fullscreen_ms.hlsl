// A mesh shader (P39) emitting the full-screen triangle of fullscreen_vs.hlsl.
// Launched directly by cmdDrawMesh, or by fullscreen_as.hlsl.
#include <equine/root.hlsli>

struct DrawRoot {
    float4 color;
    float depth;
};
EQ_ROOT_DATA(DrawRoot, root);

struct Vertex {
    float4 position : SV_Position;
};

[outputtopology("triangle")]
[numthreads(3, 1, 1)]
void main(uint thread : SV_GroupThreadID, out vertices Vertex verts[3], out indices uint3 tris[1]) {
    SetMeshOutputCounts(3, 1);
    const float2 uv = float2((thread << 1) & 2, thread & 2);
    verts[thread].position = float4(uv * float2(2, -2) + float2(-1, 1), root.depth, 1);
    if (thread == 0) {
        tris[0] = uint3(0, 1, 2);
    }
}
