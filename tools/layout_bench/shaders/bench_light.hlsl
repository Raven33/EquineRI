// "Lighting": reads albedo, normal and depth, writes lit color (storage).
#include <equine/root.hlsli>

struct LightRoot {
    uint albedoSlot;
    uint normalSlot;
    uint depthSlot;
    uint litSlot;
    uint width;
    uint height;
    uint pad[2];
};
EQ_ROOT_DATA(LightRoot, root);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= root.width || id.y >= root.height) {
        return;
    }
    Texture2D<float4> albedo = ResourceDescriptorHeap[root.albedoSlot];
    Texture2D<float4> normal = ResourceDescriptorHeap[root.normalSlot];
    Texture2D<float4> depth = ResourceDescriptorHeap[root.depthSlot];
    RWTexture2D<float4> lit = ResourceDescriptorHeap[root.litSlot];
    const float3 n = normalize(normal.Load(int3(id.xy, 0)).xyz + 1e-4f);
    const float d = depth.Load(int3(id.xy, 0)).x;
    const float light = saturate(dot(n, normalize(float3(0.3f, 0.8f, 0.5f))));
    lit[id.xy] = float4(albedo.Load(int3(id.xy, 0)).rgb * light * (1.0f - d * 0.5f), 1);
}
