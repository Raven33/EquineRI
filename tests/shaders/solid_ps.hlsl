// Writes root.color to the first color attachment.
#include <equine/root.hlsli>

struct DrawRoot {
    float4 color;
    float depth;
};
EQ_ROOT_DATA(DrawRoot, root);

float4 main() : SV_Target0 { return root.color; }
