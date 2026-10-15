// The linear group index of a grid from RI::Helpers::groupGrid (CONTEXT.md
// P106): grids above 65,535 groups are folded into rows of 65,535. Groups past
// the end of the last row exist; compare the index with the group count.
//
//     [numthreads(64, 1, 1)] void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
//         const uint item = eqLinearGroup(group) * 64 + thread.x;
//         if (item >= root.count) return;
//         ...
//     }
#ifndef EQUINE_GRID_HLSLI
#define EQUINE_GRID_HLSLI

uint eqLinearGroup(uint3 groupId) { return groupId.x + groupId.y * 65535u; }

#endif
