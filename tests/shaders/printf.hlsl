// Shader printf (P53 (3)): Vulkan validation builds print it to stdout. DXIL
// has no printf yet (a D3D12 version is a later helper), so it is SPIR-V only.
[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
#ifdef __spirv__
    printf("EquineRI printf %u %u", 42u, id.x);
#endif
}
