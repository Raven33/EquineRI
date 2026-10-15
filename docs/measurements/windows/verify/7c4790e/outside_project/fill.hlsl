#include <equine/root.hlsli>

struct Root {
    uint outputSlot;
    uint count;
};
EQ_ROOT_DATA(Root, root);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= root.count) {
        return;
    }
    RWStructuredBuffer<uint> output = ResourceDescriptorHeap[root.outputSlot];
    output[id.x] = id.x * 2 + 1;
}
