// No root data: only declared resources, including an unbounded array and an
// ordinary constant buffer.
struct Params {
    uint count;
    float3 pad;
};
ConstantBuffer<Params> params : register(b0);
RWStructuredBuffer<uint> b : register(u0, space1);
Texture2D t[] : register(t0, space2);
SamplerState s : register(s3);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= params.count) {
        return;
    }
    const float4 texel = t[NonUniformResourceIndex(id.x)].SampleLevel(s, float2(0.5f, 0.5f), 0.0f);
    b[id.x] = uint(texel.x);
}
