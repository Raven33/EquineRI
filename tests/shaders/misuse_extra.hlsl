// Misuse (P45): root data plus another resource in the reserved space 999.
#include <equine/root.hlsli>

struct ExtraRoot {
    uint value;
};
EQ_ROOT_DATA(ExtraRoot, root);
RWStructuredBuffer<uint> intruder : register(u0, space999);

[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    intruder[id.x] = root.value;
}
