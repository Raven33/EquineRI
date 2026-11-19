// Per-primitive pixel inputs (mesh pipelines). D3D12 links them to the mesh
// shader's `out primitives` by semantic: the pixel shader declares them like
// any other input. Vulkan needs the fragment input decorated PerPrimitiveEXT,
// which DXC cannot infer from the pixel shader alone; RI adds the decoration
// when it creates the pipeline (it sees both stages). These macros say it in
// the shader as well, for pipelines created elsewhere or to be explicit:
//
//     struct PSIn {
//         float4 position : SV_Position;
//         EQ_PER_PRIMITIVE nointerpolation uint face : FACE;
//     };
//     EQ_PER_PRIMITIVE_MAIN
//     float4 main(PSIn input) : SV_Target0 { ... }
//
// Both are empty when compiling to DXIL.
#ifndef EQUINE_PERPRIMITIVE_HLSLI
#define EQUINE_PERPRIMITIVE_HLSLI

#ifdef __spirv__
#define EQ_PER_PRIMITIVE [[vk::ext_decorate(5271)]] // PerPrimitiveEXT
#define EQ_PER_PRIMITIVE_MAIN [[vk::ext_capability(5283)]] [[vk::ext_extension("SPV_EXT_mesh_shader")]] // MeshShadingEXT
#else
#define EQ_PER_PRIMITIVE
#define EQ_PER_PRIMITIVE_MAIN
#endif

#endif
