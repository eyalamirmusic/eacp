struct VertexIn
{
    float2 a0 : TEXCOORD0;
};

struct VertexOut
{
    float4 position : SV_Position;
    float2 v0 : TEXCOORD0;
};

struct Uniforms
{
    float2 u0;
    float4 u1;
    float4 u2;
};

cbuffer UniformsCB : register(b0)
{
    Uniforms uniforms;
};

Texture2D texture0 : register(t0);
SamplerState samplerConfig0 : register(s0);

VertexOut vertexMain(VertexIn input)
{
    VertexOut output;
    output.position = float4(((((uniforms.u1).x + ((input.a0).x * (uniforms.u1).z)) / ((uniforms.u0).x * 0.5)) - 1.0), (1.0 - (((uniforms.u1).y + ((input.a0).y * (uniforms.u1).w)) / ((uniforms.u0).y * 0.5))), 0.0, 1.0);
    output.v0 = input.a0;
    return output;
}

float4 fragmentMain(VertexOut input) : SV_Target
{
    return float4((uniforms.u2).xyz, ((uniforms.u2).w * (texture0.Sample(samplerConfig0, input.v0)).x));
}
