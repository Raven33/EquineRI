// 64-bit atomics on a heap buffer and on groupshared memory (P105). Thread i
// adds and maxes v(i) = ((i + 1) << 32) | 7: buffer[0] gets the sum, buffer[1]
// the largest value seen by any group.
#include <equine/root.hlsli>

struct Atomic64Root {
    uint bufferSlot;
};
EQ_ROOT_DATA(Atomic64Root, root);

groupshared uint64_t groupMax;

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex) {
    RWStructuredBuffer<uint64_t> buffer = ResourceDescriptorHeap[root.bufferSlot];
    if (index == 0) {
        groupMax = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    const uint64_t value = (uint64_t(id.x + 1) << 32) | 7;
    InterlockedMax(groupMax, value);
    InterlockedAdd(buffer[0], value);
    GroupMemoryBarrierWithGroupSync();
    if (index == 0) {
        InterlockedMax(buffer[1], groupMax);
    }
}
