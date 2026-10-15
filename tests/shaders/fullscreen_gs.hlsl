// Expands each point into the full-screen triangle, passing its color on.
struct VertexOut {
    float4 position : SV_Position;
    float4 color : COLOR0;
};

[maxvertexcount(3)]
void main(point VertexOut input[1], inout TriangleStream<VertexOut> stream) {
    for (uint vertex = 0; vertex < 3; ++vertex) {
        const float2 uv = float2((vertex << 1) & 2, vertex & 2);
        VertexOut o;
        o.position = float4(uv * float2(2, -2) + float2(-1, 1), input[0].position.z, 1);
        o.color = input[0].color;
        stream.Append(o);
    }
}
