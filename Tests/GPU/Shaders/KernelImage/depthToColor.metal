#include <metal_stdlib>
using namespace metal;

struct VertexIn
{
    float2 position [[attribute(0)]];
};

struct VertexOut
{
    float4 position [[position]];
};

vertex VertexOut vertexMain(VertexIn in [[stage_in]])
{
    VertexOut out;
    out.position = float4(in.position, 0.5, 1.0);
    return out;
}

fragment float4 fragmentMain(VertexOut in [[stage_in]],
                             depth2d<float, access::read> depth [[texture(0)]])
{
    return float4(depth.read(uint2(in.position.xy)), 0.0, 0.0, 1.0);
}
