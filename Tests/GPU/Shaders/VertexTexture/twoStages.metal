#include <metal_stdlib>
using namespace metal;

struct VertexIn
{
    float2 position [[attribute(0)]];
};

struct VertexOut
{
    float4 position [[position]];
    float4 fromVertex;
};

vertex VertexOut vertexMain(VertexIn in [[stage_in]],
                            texture2d<float> vertexImage [[texture(12)]],
                            sampler vertexSampler [[sampler(12)]])
{
    VertexOut out;
    out.position = float4(in.position, 0.0, 1.0);
    out.fromVertex = vertexImage.sample(vertexSampler, float2(0.5), level(0));
    return out;
}

fragment float4 fragmentMain(VertexOut in [[stage_in]],
                             texture2d<float> fragmentImage [[texture(12)]],
                             sampler fragmentSampler [[sampler(12)]])
{
    float4 fromFragment = fragmentImage.sample(fragmentSampler, float2(0.5));
    return float4(in.fromVertex.rgb + fromFragment.rgb, 1.0);
}
