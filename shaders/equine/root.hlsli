// Root data and bindful constant buffers (CONTEXT.md P45, P112). Include this
// header and declare the shader's root data once:
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

// Bindful constant buffers (P112): slot 0 to 7, bound with
// cmdSetConstantBuffer(cmd, slot, buffer, offset).
//
//     struct Pass { float4x4 viewProjection; };
//     EQ_CONSTANT_BUFFER(Pass, pass, 0);
//
// Vulkan: a uniform buffer at set 2, binding `slot`, which RI fills with push
// descriptors. D3D12: register(b<slot>, space998), which RI makes root CBVs.
// Nothing else may use space 998 or set 2. `slot` may be a macro.
#define EQ_REGISTER_B_(slot) b##slot
#define EQ_REGISTER_B(slot) EQ_REGISTER_B_(slot)
#ifdef __spirv__
#define EQ_CONSTANT_BUFFER(Type, name, slot) [[vk::binding(slot, 2)]] ConstantBuffer<Type> name
#else
#define EQ_CONSTANT_BUFFER(Type, name, slot) ConstantBuffer<Type> name : register(EQ_REGISTER_B(slot), space998)
#endif

#endif
