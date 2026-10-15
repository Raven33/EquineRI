// Compute pass of the statistics-cost workload (P105): arithmetic written to a
// buffer, so compute invocations are counted too.
#include <equine/root.hlsli>

struct ComputeRoot {
    uint bufferSlot;
    uint count;
};
EQ_ROOT_DATA(ComputeRoot, root);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= root.count) {
        return;
    }
    RWStructuredBuffer<float> output = ResourceDescriptorHeap[root.bufferSlot];
    float v = id.x;
    [unroll] for (uint i = 0; i < 16; ++i) {
        v = frac(sin(v * 12.9898 + i) * 43758.5453);
    }
    output[id.x] = v;
}
