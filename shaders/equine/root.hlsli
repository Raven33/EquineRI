// Root data marking (CONTEXT.md P45). Include this header and declare the
// shader's root data once:
//
//     struct FillRoot { uint output; float value; };
//     EQ_ROOT_DATA(FillRoot, root);
//
// Vulkan: a push-constant block. D3D12: a constant buffer in the reserved
// register space 999, which RI turns into root constants (P67). Nothing else
// may use space 999.
#ifndef EQUINE_ROOT_HLSLI
#define EQUINE_ROOT_HLSLI

#ifdef __spirv__
#define EQ_ROOT_DATA(Type, name) [[vk::push_constant]] ConstantBuffer<Type> name
#else
#define EQ_ROOT_DATA(Type, name) ConstantBuffer<Type> name : register(b0, space999)
#endif

#endif
