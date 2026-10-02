struct VertexIn
{
    float2 a0 : TEXCOORD0;
    float4 a1 : TEXCOORD1;
    float4 a2 : TEXCOORD2;
    float4 a3 : TEXCOORD3;
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
    float2 u1;
};

cbuffer UniformsCB : register(b0)
{
    Uniforms uniforms;
};

Texture2D texture0 : register(t0);
SamplerState samplerConfig2 : register(s2);

VertexOut vertexMain(VertexIn input)
{
    float2 t0 = float2(((input.a1).x + ((input.a0).x * (input.a1).z)), ((input.a1).y + ((input.a0).y * (input.a1).w)));
    VertexOut output;
    output.position = float4(((((t0).x / (uniforms.u0).x) * 2.0) - 1.0), (1.0 - (((t0).y / (uniforms.u0).y) * 2.0)), 0.0, 1.0);
    output.v0 = float2((((input.a2).x + ((input.a0).x * (input.a2).z)) / (uniforms.u1).x), (((input.a2).y + ((input.a0).y * (input.a2).w)) / (uniforms.u1).y));
    output.v1 = input.a3;
    return output;
}

float4 fragmentMain(VertexOut input) : SV_Target
{
    float4 t0 = texture0.Sample(samplerConfig2, input.v0);
    return float4((t0).x, (t0).y, (t0).z, ((t0).w * (input.v1).w));
}
