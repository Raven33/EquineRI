// fullscreen_ms.hlsl for a pipeline with fullscreen_as.hlsl: D3D12 requires the
// mesh shader to take exactly the payload the task shader sends.
#include <equine/root.hlsli>

struct DrawRoot {
    float4 color;
    float depth;
};
EQ_ROOT_DATA(DrawRoot, root);

struct Vertex {
    float4 position : SV_Position;
};

struct Payload {
    uint unused;
};

[outputtopology("triangle")]
[numthreads(3, 1, 1)]
void main(uint thread : SV_GroupThreadID, in payload Payload payload, out vertices Vertex verts[3],
          out indices uint3 tris[1]) {
    SetMeshOutputCounts(3, 1);
    const float2 uv = float2((thread << 1) & 2, thread & 2);
    verts[thread].position = float4(uv * float2(2, -2) + float2(-1, 1), root.depth, 1);
    if (thread == 0) {
        tris[0] = uint3(0, 1, 2);
    }
}
