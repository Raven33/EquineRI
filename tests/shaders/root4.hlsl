// Root data of a single uint (4 bytes); the output is reached through the heap.
#include <equine/root.hlsli>

struct SlotRoot {
    uint outputSlot;
};
EQ_ROOT_DATA(SlotRoot, root);

[numthreads(8, 4, 2)]
void main(uint3 id : SV_DispatchThreadID) {
    RWStructuredBuffer<uint> output = ResourceDescriptorHeap[root.outputSlot];
    output[id.x] = id.y;
}
