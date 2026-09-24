struct VertexIn
{
    float2 a0 : TEXCOORD0;
    float2 a1 : TEXCOORD1;
    float2 a2 : TEXCOORD2;
    float2 a3 : TEXCOORD3;
    float2 a4 : TEXCOORD4;
    float2 a5 : TEXCOORD5;
    float4 a6 : TEXCOORD6;
};

struct VertexOut
{
    float4 position : SV_Position;
    float2 v0 : TEXCOORD0;
    float2 v1 : TEXCOORD1;
    float4 v2 : TEXCOORD2;
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
Texture2D texture1 : register(t1);
SamplerState samplerConfig0 : register(s0);
SamplerState samplerConfig2 : register(s2);

VertexOut vertexMain(VertexIn input)
{
    float2 t0 = ((input.a1 + ((input.a0).x * input.a2)) + ((input.a0).y * input.a3));
    VertexOut output;
    output.position = float4(((((t0).x / (uniforms.u0).x) * 2.0) - 1.0), (1.0 - (((t0).y / (uniforms.u0).y) * 2.0)), 0.0, 1.0);
    output.v0 = (input.a4 + (input.a0 * (input.a5 - input.a4)));
    output.v1 = t0;
    output.v2 = input.a6;
    return output;
}

float4 fragmentMain(VertexOut input) : SV_Target
{
    float4 t0 = (texture0.Sample(samplerConfig2, input.v0) * input.v2);
    float2 t1 = ((input.v1 - (uniforms.u1).xy) * (uniforms.u1).zw);
    float2 t2 = abs((t1 - 0.5));
    return float4((t0).x, (t0).y, (t0).z, ((t0).w * ((step((t2).x, 0.5) * step((t2).y, 0.5)) * (texture1.Sample(samplerConfig0, ((uniforms.u2).xy + (t1 * (uniforms.u2).zw)))).x)));
}
