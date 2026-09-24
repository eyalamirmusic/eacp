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
    float2 u1;
    float2 u2;
    float2 u3;
    float4 u4;
    float4 u5;
    float4 u6;
};

cbuffer UniformsCB : register(b0)
{
    Uniforms uniforms;
};

Texture2D texture0 : register(t0);
Texture2D texture1 : register(t1);
SamplerState samplerConfig1 : register(s1);

VertexOut vertexMain(VertexIn input)
{
    float2 t0 = ((uniforms.u1 + ((input.a0).x * uniforms.u2)) + ((input.a0).y * uniforms.u3));
    VertexOut output;
    output.position = float4(((((t0).x / (uniforms.u0).x) * 2.0) - 1.0), (1.0 - (((t0).y / (uniforms.u0).y) * 2.0)), 0.0, 1.0);
    output.v0 = input.a0;
    return output;
}

float4 fragmentMain(VertexOut input) : SV_Target
{
    float t0 = (((texture0.Sample(samplerConfig1, input.v0)).x - (uniforms.u5).x) * (uniforms.u5).y);
    float4 t1 = texture1.Sample(samplerConfig1, input.v0);
    float t2 = (((t1).y - (uniforms.u5).z) * (uniforms.u5).w);
    float t3 = (((t1).x - (uniforms.u5).z) * (uniforms.u5).w);
    float3 t4 = clamp(float3((t0 + ((uniforms.u6).x * t2)), ((t0 - ((uniforms.u6).y * t3)) - ((uniforms.u6).z * t2)), (t0 + ((uniforms.u6).w * t3))), 0.0, 1.0);
    return (float4((t4).x, (t4).y, (t4).z, 1.0) * uniforms.u4);
}
