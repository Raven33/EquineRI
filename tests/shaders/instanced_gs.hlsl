// Geometry-shader instancing: two instances per point, instance i covering
// half i of the target (left, right) in the point's color times (i + 1) / 2.
struct VertexOut {
    float4 position : SV_Position;
    float4 color : COLOR0;
};

[instance(2)]
[maxvertexcount(4)]
void main(point VertexOut input[1], uint instance : SV_GSInstanceID, inout TriangleStream<VertexOut> stream) {
    const float left = instance == 0 ? -1.0 : 0.0;
    for (uint vertex = 0; vertex < 4; ++vertex) {
        VertexOut o;
        o.position = float4(left + (vertex & 1), (vertex & 2) ? -1.0 : 1.0, input[0].position.z, 1);
        o.color = input[0].color * ((instance + 1) * 0.5);
        stream.Append(o);
    }
}
