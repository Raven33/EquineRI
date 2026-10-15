// Pixel shader of the statistics-cost workload (P105): a little arithmetic per
// pixel, so the pass is not only rasterization.
struct Vertex {
    float4 position : SV_Position;
    float3 color : COLOR0;
};

float4 main(Vertex input) : SV_Target0 {
    float3 c = input.color;
    [unroll] for (uint i = 0; i < 8; ++i) {
        c = frac(sin(c * 12.9898 + i) * 43758.5453);
    }
    return float4(c, 1);
}
