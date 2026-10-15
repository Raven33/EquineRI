// Passes the three control points on, with tessellation factors of 1.
struct ControlPoint {
    float4 position : POSITION0;
};

struct PatchConstants {
    float edges[3] : SV_TessFactor;
    float inside : SV_InsideTessFactor;
};

PatchConstants constants(InputPatch<ControlPoint, 3> patch) {
    PatchConstants o;
    o.edges[0] = 1;
    o.edges[1] = 1;
    o.edges[2] = 1;
    o.inside = 1;
    return o;
}

[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("constants")]
ControlPoint main(InputPatch<ControlPoint, 3> patch, uint id : SV_OutputControlPointID) { return patch[id]; }
