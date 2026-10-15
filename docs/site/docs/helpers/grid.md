# Group grids

## What it does

A dispatch or mesh draw launches at most 65,535 groups per dimension on every backend
(`kMaxGroupsPerDimension`, [P106](../design-notes/decisions.md#p106)). `<RI/Helpers/Grid.hpp>`
folds a larger count into rows. `groupGrid(count, groupSize)` returns the groups for `count`
items of `groupSize` each, as `DispatchIndirectArgs`:

- up to 65,535 groups: `{groups, 1, 1}`;
- above: rows of 65,535 groups, `{65535, rows, 1}`, up to 65,535 rows.

The shader gets the linear group index back with `eqLinearGroup` from `<equine/grid.hlsli>`.
The last row can launch groups past the end, so the shader compares the index with the count.
The arguments can also be written into an indirect buffer, since they have the same layout.

It runs nothing on the GPU and keeps no state.

## Example

```cpp
const RI::DispatchIndirectArgs grid = RI::Helpers::groupGrid(itemCount, 64);
RI::Helpers::cmdSetRootData(cmd, Root{.count = itemCount});
RI::cmdDispatch(cmd, grid.x, grid.y, grid.z);
```

```hlsl
#include <equine/grid.hlsli>
#include <equine/root.hlsli>

[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
    const uint item = eqLinearGroup(group) * 64 + thread.x;
    if (item >= root.count) {
        return;
    }
    // ...
}
```

Mesh draws also have a total of `kMaxMeshGroups` (2^22) groups; a folded grid stays within it
only below that many groups.

## How to roll your own

Divide the group count by 65,535 and round up for the rows. In the shader, the linear index is
`group.x + group.y * 65535`.
