#include <metal_stdlib>
using namespace metal;

struct VertexIn
{
    float3 column [[attribute(0)]];
};

struct VertexOut
{
    float4 position [[position]];
};

vertex VertexOut vertexMain(VertexIn in [[stage_in]],
                            depth2d<float> depthImage [[texture(0)]],
                            sampler depthSampler [[sampler(0)]])
{
    float2 uv = float2(in.column.y, 0.5);
    float passed = depthImage.sample_compare(depthSampler, uv, 0.5, level(0));
    float h = 0.5 + 0.5 * passed;

    VertexOut out;
    out.position = float4(in.column.x, -1.0 + 2.0 * h * in.column.z, 0.0, 1.0);
    return out;
}

fragment float4 fragmentMain(VertexOut in [[stage_in]])
{
    return float4(1.0);
}
