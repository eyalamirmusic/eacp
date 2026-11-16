#include <metal_stdlib>
using namespace metal;

struct VertexIn
{
    float3 position [[attribute(0)]];
    float4 color [[attribute(1)]];
};

struct VertexOut
{
    float4 position [[position]];
    float4 color;
    uint layer [[render_target_array_index]];
};

vertex VertexOut vertexMain(VertexIn in [[stage_in]],
                            uint instance [[instance_id]])
{
    VertexOut out;
    out.position = float4(in.position, 1.0);
    out.color = float4(float(instance + 1) * 0.2, 0.0, 0.0, 1.0);
    out.layer = instance;
    return out;
}

fragment float4 fragmentMain(VertexOut in [[stage_in]])
{
    return in.color;
}
