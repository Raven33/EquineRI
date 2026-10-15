// Expands one point into six full-screen triangles, one per layer of the
// render target, face f in the color (f / 8 + 1 / 16, 1 - f / 8, 0, 1).
struct VertexOut {
    float4 position : SV_Position;
    float4 color : COLOR0;
};

struct LayerOut {
    float4 position : SV_Position;
    float4 color : COLOR0;
    uint layer : SV_RenderTargetArrayIndex;
};

[maxvertexcount(18)]
void main(point VertexOut input[1], inout TriangleStream<LayerOut> stream) {
    for (uint face = 0; face < 6; ++face) {
        for (uint vertex = 0; vertex < 3; ++vertex) {
            const float2 uv = float2((vertex << 1) & 2, vertex & 2);
            LayerOut o;
            o.position = float4(uv * float2(2, -2) + float2(-1, 1), input[0].position.z, 1);
            o.color = float4(face / 8.0 + 1.0 / 16.0, 1.0 - face / 8.0, 0, 1);
            o.layer = face;
            stream.Append(o);
        }
        stream.RestartStrip();
    }
}
