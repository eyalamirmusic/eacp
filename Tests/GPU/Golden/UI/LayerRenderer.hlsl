struct VertexIn
{
    float2 a0 : TEXCOORD0;
};

struct VertexOut
{
    float4 position : SV_Position;
    float2 v0 : TEXCOORD0;
    float2 v1 : TEXCOORD1;
};

struct Uniforms
{
    float2 u0;
    float4 u1;
    float4 u2;
    float u3;
    float4 u4;
    float2 u5;
    float4 u6;
    float4 u7;
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
    float2 t0 = float2(((input.a0).x * (uniforms.u1).z), ((input.a0).y * (uniforms.u1).w));
    float2 t1 = float2(((((uniforms.u4).x * (t0).x) + ((uniforms.u4).z * (t0).y)) + (uniforms.u5).x), ((((uniforms.u4).y * (t0).x) + ((uniforms.u4).w * (t0).y)) + (uniforms.u5).y));
    float2 t2 = float2(((uniforms.u1).x + (t1).x), ((uniforms.u1).y + (t1).y));
    VertexOut output;
    output.position = float4(((((t2).x / (uniforms.u0).x) * 2.0) - 1.0), (1.0 - (((t2).y / (uniforms.u0).y) * 2.0)), 0.0, 1.0);
    output.v0 = float2(((uniforms.u2).x + ((input.a0).x * (uniforms.u2).z)), ((uniforms.u2).y + ((input.a0).y * (uniforms.u2).w)));
    output.v1 = t2;
    return output;
}

float4 fragmentMain(VertexOut input) : SV_Target
{
    float4 t0 = texture0.Sample(samplerConfig2, input.v0);
    float3 t1 = ((t0).xyz / max((t0).w, 0.00195312));
    float2 t2 = ((input.v1 - (uniforms.u6).xy) * (uniforms.u6).zw);
    float2 t3 = abs((t2 - 0.5));
    return float4((t1).x, (t1).y, (t1).z, (((t0).w * uniforms.u3) * ((step((t3).x, 0.5) * step((t3).y, 0.5)) * (texture1.Sample(samplerConfig0, ((uniforms.u7).xy + (t2 * (uniforms.u7).zw)))).x)));
}
