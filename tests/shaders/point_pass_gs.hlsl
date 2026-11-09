// Passes each point on unchanged.
struct VertexOut {
    float4 position : SV_Position;
    float4 color : COLOR0;
};

[maxvertexcount(1)]
void main(point VertexOut input[1], inout PointStream<VertexOut> stream) { stream.Append(input[0]); }
