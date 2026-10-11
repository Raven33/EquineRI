// Inline ray tracing (P49): one ray per pixel along +z from z = -1, through
// the top level in heap slot root.tlasSlot; writes the hit distance, or -1.
#include <equine/root.hlsli>
struct TraceRoot { uint tlasSlot; uint outputSlot; uint width; uint height; };
EQ_ROOT_DATA(TraceRoot, root);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    RaytracingAccelerationStructure tlas = ResourceDescriptorHeap[root.tlasSlot];
    RWStructuredBuffer<float> output = ResourceDescriptorHeap[root.outputSlot];
    RayDesc ray;
    ray.Origin = float3((id.x + 0.5) / root.width * 2 - 1, (id.y + 0.5) / root.height * 2 - 1, -1);
    ray.Direction = float3(0, 0, 1);
    ray.TMin = 0; ray.TMax = 10;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(tlas, RAY_FLAG_NONE, 0xff, ray);
    q.Proceed();
    output[id.y * root.width + id.x] = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? q.CommittedRayT() : -1;
}
