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
    float4 v1 : TEXCOORD1;
};

struct Uniforms
{
    float2 u0;
};

cbuffer UniformsCB : register(b0)
{
    Uniforms uniforms;
};

Texture2D texture0 : register(t0);
SamplerState samplerConfig1 : register(s1);

VertexOut vertexMain(VertexIn input)
{
    float2 t0 = ((input.a1 + ((input.a0).x * input.a2)) + ((input.a0).y * input.a3));
    VertexOut output;
    output.position = float4(((((t0).x / (uniforms.u0).x) * 2.0) - 1.0), (1.0 - (((t0).y / (uniforms.u0).y) * 2.0)), 0.0, 1.0);
    output.v0 = (input.a4 + (input.a0 * (input.a5 - input.a4)));
    output.v1 = input.a6;
    return output;
}

float4 fragmentMain(VertexOut input) : SV_Target
{
    return (texture0.Sample(samplerConfig1, input.v0) * input.v1);
}
