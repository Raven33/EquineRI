// Reads only NORMAL from link_vs, leaving out SV_Position and TEXCOORD0 before
// it: register 0 on D3D12 and location 0 on Vulkan, where link_vs writes
// another value (P101). SV_IsFrontFace is made by the GPU, not link_vs.
struct PSIn {
    float3 normal : NORMAL;
    bool front : SV_IsFrontFace;
};

float4 main(PSIn input) : SV_Target0 { return float4(input.front ? input.normal : -input.normal, 1); }
