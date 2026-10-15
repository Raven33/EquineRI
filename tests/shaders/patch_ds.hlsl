// Places each tessellated vertex inside the patch's triangle; the color comes
// from root data.
#include <equine/root.hlsli>

struct DrawRoot {
    float4 color;
    float depth;
};
EQ_ROOT_DATA(DrawRoot, root);

struct ControlPoint {
    float4 position : POSITION0;
};

struct PatchConstants {
    float edges[3] : SV_TessFactor;
    float inside : SV_InsideTessFactor;
};

struct VertexOut {
    float4 position : SV_Position;
    float4 color : COLOR0;
};

[domain("tri")]
VertexOut main(PatchConstants constants, float3 barycentric : SV_DomainLocation,
               const OutputPatch<ControlPoint, 3> patch) {
    VertexOut o;
    o.position = patch[0].position * barycentric.x + patch[1].position * barycentric.y +
                 patch[2].position * barycentric.z;
    o.color = root.color;
    return o;
}
