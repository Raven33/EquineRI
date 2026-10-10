// A position-dependent pattern into one target.
#include <equine/root.hlsli>

struct DrawRoot {
    float depth;
    float tilt;
    uint seed;
    uint pad;
};
EQ_ROOT_DATA(DrawRoot, root);


float hash(uint2 p, uint s) {
    uint h = p.x * 73856093u ^ p.y * 19349663u ^ s * 83492791u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return (h & 0xffffu) / 65535.0f;
}

float4 main(float4 position : SV_Position, nointerpolation uint instance : INSTANCE) : SV_Target0 {
    const uint2 p = uint2(position.xy);
    return float4(hash(p, root.seed + instance), hash(p / 4, instance), hash(p / 16, root.seed), 1);
}
