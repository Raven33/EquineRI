// The pixel shader of the documentation's rendering examples: writes the
// root data's color to color attachment 0 (profile ps_6_6).

// --8<-- [start:main]
#include <equine/root.hlsli>

struct DrawRoot {
    float4 color;
    float depth;
};
EQ_ROOT_DATA(DrawRoot, root); // the same root data as the vertex shader (P44)

float4 main() : SV_Target0 { return root.color; }
// --8<-- [end:main]
