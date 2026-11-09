// Indirect Level 2 dispatches (P48): each record's root data names a slot of
// the output buffer and the value to write there.
#include <equine/root.hlsli>

struct IndirectWriteRoot {
    uint outputSlot;
    uint index;
    uint value;
};
EQ_ROOT_DATA(IndirectWriteRoot, root);

[numthreads(1, 1, 1)]
void main() {
    RWStructuredBuffer<uint> output = ResourceDescriptorHeap[root.outputSlot];
    output[root.index] = root.value;
}
