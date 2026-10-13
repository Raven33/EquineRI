// Passes three values to the pixel shader after SV_Position (P101): registers
// 1 to 3 on D3D12, locations 0 to 2 on Vulkan. No input: vertex pulling (P39).
struct VSOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float3 normal : NORMAL;
    float3 color : COLOR1;
};

VSOut main(uint vertex : SV_VertexID) {
    VSOut result;
    result.position = float4(float(vertex), 0, 0, 1);
    result.uv = float2(0, 1);
    result.normal = float3(0, 0, 1);
    result.color = float3(1, 0, 0);
    return result;
}
