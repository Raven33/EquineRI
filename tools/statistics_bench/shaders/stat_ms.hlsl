// Mesh shader of the statistics-cost workload (P105): each group outputs an
// 8x8-vertex grid (98 triangles) over one screen tile, at a depth that varies
// per meshlet and pass, so meshlets overlap.
#include <equine/root.hlsli>

struct MeshRoot {
    uint gridSize;
    uint pass;
    uint meshletsPerTask;
};
EQ_ROOT_DATA(MeshRoot, root);

struct Payload {
    uint firstMeshlet;
};

struct Vertex {
    float4 position : SV_Position;
    float3 color : COLOR0;
};

[outputtopology("triangle")]
[numthreads(64, 1, 1)]
void main(uint thread : SV_GroupThreadID, uint3 group : SV_GroupID, in payload Payload payload,
          out vertices Vertex verts[64], out indices uint3 tris[98]) {
    SetMeshOutputCounts(64, 98);
    const uint meshlet = payload.firstMeshlet + group.x;
    const uint tiles = root.gridSize;
    const float2 tile = float2(meshlet % tiles, (meshlet / tiles) % tiles);
    const float2 corner = float2(thread % 8, thread / 8) / 7.0;
    const float2 position = float2(-1, -1) + (tile + corner) * (2.0 / tiles);
    const float depth = frac(meshlet * 0.618034 + root.pass * 0.1) * 0.9 + 0.05;
    verts[thread].position = float4(position, depth, 1);
    verts[thread].color = float3(corner, depth);
    for (uint t = thread; t < 98; t += 64) {
        const uint quad = t / 2;
        const uint first = (quad / 7) * 8 + quad % 7;
        tris[t] = (t & 1) == 0 ? uint3(first, first + 1, first + 8) : uint3(first + 1, first + 9, first + 8);
    }
}
