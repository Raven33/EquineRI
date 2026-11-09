// Red in layer 0, green in the others.
float4 main(float4 position : SV_Position, uint layer : SV_RenderTargetArrayIndex) : SV_Target0 {
    return layer == 0 ? float4(1, 0, 0, 1) : float4(0, 1, 0, 1);
}
