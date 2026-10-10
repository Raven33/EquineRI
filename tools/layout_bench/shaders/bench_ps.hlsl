// A position-dependent pattern into two targets (G-buffer: albedo, normal).
#include <equine/root.hlsli>

struct DrawRoot {
    float depth;
    float tilt;
    uint seed;
    uint pad;
};
EQ_ROOT_DATA(DrawRoot, root);

struct PsOut {
    float4 color0 : SV_Target0;
    float4 color1 : SV_Target1;
};

float hash(uint2 p, uint s) {
    uint h = p.x * 73856093u ^ p.y * 19349663u ^ s * 83492791u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return (h & 0xffffu) / 65535.0f;
}

PsOut main(float4 position : SV_Position, nointerpolation uint instance : INSTANCE) {
    const uint2 p = uint2(position.xy);
    PsOut o;
    o.color0 = float4(hash(p, root.seed + instance), hash(p / 4, instance), hash(p / 16, root.seed), 1);
    o.color1 = float4(hash(p, 7 + instance) * 2 - 1, hash(p, 11) * 2 - 1, 0.5f, 1);
    return o;
}
