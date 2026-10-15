// Writes the color the previous stage passes in COLOR0.
struct VertexOut {
    float4 position : SV_Position;
    float4 color : COLOR0;
};

float4 main(VertexOut input) : SV_Target0 { return input.color; }
