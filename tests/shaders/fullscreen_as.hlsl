// A task (amplification) shader (P39) that launches one fullscreen_ms group.
#include <equine/root.hlsli>

struct DrawRoot {
    float4 color;
    float depth;
};
EQ_ROOT_DATA(DrawRoot, root);

struct Payload {
    uint unused;
};
groupshared Payload payload;

[numthreads(1, 1, 1)]
void main() {
    payload.unused = 0;
    DispatchMesh(1, 1, 1, payload);
}
