// Misuse (P45): a constant buffer in the reserved space 999 other than b0.
struct Misplaced {
    uint value;
};
ConstantBuffer<Misplaced> misplaced : register(b1, space999);

[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    RWStructuredBuffer<uint> output = ResourceDescriptorHeap[0];
    output[id.x] = misplaced.value;
}
