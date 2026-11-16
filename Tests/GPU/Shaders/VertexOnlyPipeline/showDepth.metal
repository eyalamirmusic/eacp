#include <metal_stdlib>
using namespace metal;

struct VertexIn
{
    float2 position [[attribute(0)]];
    float2 uv [[attribute(1)]];
};

struct VertexOut
{
    float4 position [[position]];
    float2 uv;
};

vertex VertexOut vertexMain(VertexIn in [[stage_in]])
{
    VertexOut out;
    out.position = float4(in.position, 0.0, 1.0);
    out.uv = in.uv;
    return out;
}

fragment float4 fragmentMain(VertexOut in [[stage_in]],
                             depth2d<float> depthImage [[texture(0)]],
                             sampler depthSampler [[sampler(0)]])
{
    return float4(depthImage.sample(depthSampler, in.uv), 0.0, 0.0, 1.0);
}
