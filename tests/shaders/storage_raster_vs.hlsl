// A triangle covering the screen, for pixel shaders that write only storage
// (sections without attachments, P105). Same root data as storage_raster_ps.
#include <equine/root.hlsli>

struct RasterRoot {
    uint bufferSlot;
    uint width;
    uint value;
};
EQ_ROOT_DATA(RasterRoot, root);

float4 main(uint vertex : SV_VertexID) : SV_Position {
    const float2 uv = float2((vertex << 1) & 2, vertex & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1);
}
