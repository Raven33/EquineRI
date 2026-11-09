// Layered rendering from the vertex shader: instance i draws the full-screen
// triangle of fullscreen_vs.hlsl into layer i (SV_RenderTargetArrayIndex).
struct VertexOut {
    float4 position : SV_Position;
    uint layer : SV_RenderTargetArrayIndex;
};

VertexOut main(uint v : SV_VertexID, uint instance : SV_InstanceID) {
    VertexOut o;
    const float2 uv = float2((v << 1) & 2, v & 2);
    o.position = float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1);
    o.layer = instance;
    return o;
}
