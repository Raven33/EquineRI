// 64-bit atomics on groupshared memory only (P105, P116 (2)): Metal reports
// int64SharedAtomics false, so creating this pipeline there is reported in
// validation builds. MSC converts it, but Metal's compiler then fails.
#include <equine/root.hlsli>

struct Atomic64SharedRoot {
    uint bufferSlot;
};
EQ_ROOT_DATA(Atomic64SharedRoot, root);

groupshared uint64_t groupMax;

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex) {
    RWStructuredBuffer<uint64_t> buffer = ResourceDescriptorHeap[root.bufferSlot];
    if (index == 0) {
        groupMax = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    InterlockedMax(groupMax, (uint64_t(id.x + 1) << 32) | 7);
    GroupMemoryBarrierWithGroupSync();
    if (index == 0) {
        buffer[id.x / 64] = groupMax;
    }
}
