// Writes the color of constant buffer slot 1 (P112).
#include <equine/root.hlsli>

struct Shape {
    float4 color;
    float depth;
};
EQ_CONSTANT_BUFFER(Shape, shape, 1);

float4 main() : SV_Target0 { return shape.color; }
