// Inline ray tracing over a scene of several geometries and instances (P49):
// one ray per pixel along +z from z = -1 (pixel row 0 at the top), with the
// ray mask from the root data; non-opaque candidates are committed. Writes per
// pixel: the hit distance's bits (~0 for a miss), the instance ID, the
// instance index | geometry index << 16, and the primitive index | front << 31.
#include <equine/root.hlsli>

struct SceneRoot {
    uint tlasSlot;
    uint outputSlot;
    uint width;
    uint height;
    uint mask;
};
EQ_ROOT_DATA(SceneRoot, root);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    RaytracingAccelerationStructure tlas = ResourceDescriptorHeap[root.tlasSlot];
    RWStructuredBuffer<uint4> output = ResourceDescriptorHeap[root.outputSlot];
    RayDesc ray;
    ray.Origin = float3((id.x + 0.5) / root.width * 2 - 1, 1 - (id.y + 0.5) / root.height * 2, -1);
    ray.Direction = float3(0, 0, 1);
    ray.TMin = 0;
    ray.TMax = 10;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(tlas, RAY_FLAG_NONE, root.mask, ray);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            q.CommitNonOpaqueTriangleHit();
        }
    }
    uint4 r = uint4(~0u, 0, 0, 0);
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        r = uint4(asuint(q.CommittedRayT()), q.CommittedInstanceID(),
                  q.CommittedInstanceIndex() | (q.CommittedGeometryIndex() << 16),
                  q.CommittedPrimitiveIndex() | (q.CommittedTriangleFrontFace() ? 0x80000000u : 0));
    }
    output[id.y * root.width + id.x] = r;
}
