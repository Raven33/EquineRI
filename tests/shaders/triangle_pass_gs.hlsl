// Passes each triangle on unchanged (after tessellation, or from a strip).
struct VertexOut {
    float4 position : SV_Position;
    float4 color : COLOR0;
};

[maxvertexcount(3)]
void main(triangle VertexOut input[3], inout TriangleStream<VertexOut> stream) {
    for (uint i = 0; i < 3; ++i) {
        stream.Append(input[i]);
    }
}
