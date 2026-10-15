// Writes no target: lowers buffer[y * width + x] to root.value with an atomic,
// as a shadow atlas writes depth (P105).
#include <equine/root.hlsli>

struct RasterRoot {
    uint bufferSlot;
    uint width;
    uint value;
};
EQ_ROOT_DATA(RasterRoot, root);

void main(float4 position : SV_Position) {
    RWStructuredBuffer<uint> buffer = ResourceDescriptorHeap[root.bufferSlot];
    const uint2 pixel = uint2(position.xy);
    InterlockedMin(buffer[pixel.y * root.width + pixel.x], root.value);
}
