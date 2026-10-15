// Task (amplification) shader of the statistics-cost workload (P105): each
// group launches `meshletsPerTask` mesh groups.
#include <equine/root.hlsli>

struct MeshRoot {
    uint gridSize;        // meshlets per row and column of the screen
    uint pass;            // shifts depths so passes overlap differently
    uint meshletsPerTask;
};
EQ_ROOT_DATA(MeshRoot, root);

struct Payload {
    uint firstMeshlet;
};
groupshared Payload payload;

[numthreads(32, 1, 1)]
void main(uint3 group : SV_GroupID, uint thread : SV_GroupIndex) {
    if (thread == 0) {
        payload.firstMeshlet = group.x * root.meshletsPerTask;
    }
    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(root.meshletsPerTask, 1, 1, payload);
}
