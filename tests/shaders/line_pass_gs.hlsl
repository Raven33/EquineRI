// Passes each line on unchanged (from lines_vs.hlsl, which writes only a position).
struct VertexOut {
    float4 position : SV_Position;
};

[maxvertexcount(2)]
void main(line VertexOut input[2], inout LineStream<VertexOut> stream) {
    stream.Append(input[0]);
    stream.Append(input[1]);
}
