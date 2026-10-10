// Full-screen triangle; instance i is pushed back in depth and tilted, so depth
// and color vary across the target (no trivially compressible content).
#include <equine/root.hlsli>

struct DrawRoot {
    float depth;
    float tilt;
    uint seed;
    uint pad;
};
EQ_ROOT_DATA(DrawRoot, root);

struct VsOut {
    float4 position : SV_Position;
    nointerpolation uint instance : INSTANCE;
};

VsOut main(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
    const float2 uv = float2((vertex << 1) & 2, vertex & 2);
    const float2 xy = uv * float2(2, -2) + float2(-1, 1);
    const float z = saturate(root.depth + 0.05f * instance + root.tilt * xy.x * 0.1f);
    VsOut o;
    o.position = float4(xy, z, 1);
    o.instance = instance;
    return o;
}
