// Takes link_vs's output struct as written: the pairing RI accepts (P101).
struct PSIn {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float3 normal : NORMAL;
    float3 color : COLOR1;
};

float4 main(PSIn input) : SV_Target0 { return float4(input.normal * input.color, input.uv.x); }
