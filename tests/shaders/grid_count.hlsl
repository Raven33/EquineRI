// Counts the groups of a folded grid (P106): each group below the count adds one.
#include <equine/grid.hlsli>
#include <equine/root.hlsli>

struct GridRoot {
    uint counterSlot; // ReadWriteView (raw) of the counter
    uint groups;
};
EQ_ROOT_DATA(GridRoot, root);

[numthreads(1, 1, 1)]
void main(uint3 group : SV_GroupID) {
    if (eqLinearGroup(group) >= root.groups) {
        return;
    }
    RWByteAddressBuffer counter = ResourceDescriptorHeap[root.counterSlot];
    counter.InterlockedAdd(0, 1);
}
