struct VertexIn
{
    float a0 : TEXCOORD0;
    float2 a1 : TEXCOORD1;
    float2 a2 : TEXCOORD2;
    float2 a3 : TEXCOORD3;
    float4 a4 : TEXCOORD4;
    float4 a5 : TEXCOORD5;
    float4 a6 : TEXCOORD6;
    float4 a7 : TEXCOORD7;
};

struct VertexOut
{
    float4 position : SV_Position;
    float4 v0 : TEXCOORD0;
    float v1 : TEXCOORD1;
    float2 v2 : TEXCOORD2;
    float4 v3 : TEXCOORD3;
    float4 v4 : TEXCOORD4;
    float v5 : TEXCOORD5;
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
    float t0 = step(0.5, input.a0);
    float t1 = step(1.5, input.a0);
    float2 t2 = lerp(lerp(input.a1, input.a2, t0), input.a3, t1);
    VertexOut output;
    output.position = float4(((((t2).x / (uniforms.u0).x) * 2.0) - 1.0), (1.0 - (((t2).y / (uniforms.u0).y) * 2.0)), 0.0, 1.0);
    output.v0 = input.a5;
    output.v1 = lerp(lerp((input.a4).x, (input.a4).y, t0), (input.a4).z, t1);
    output.v2 = t2;
    output.v3 = input.a6;
    output.v4 = input.a7;
    output.v5 = (input.a4).w;
    return output;
}

float4 fragmentMain(VertexOut input) : SV_Target
{
    float t0 = ((((input.v3).x * (input.v2).x) + ((input.v3).y * (input.v2).y)) + (input.v4).x);
    float t1 = lerp(t0, length(float2(t0, ((((input.v3).z * (input.v2).x) + ((input.v3).w * (input.v2).y)) + (input.v4).y))), step(1.5, input.v5));
    float4 t2 = lerp(input.v0, texture1.Sample(samplerConfig2, float2((0.00195312 + (lerp(lerp(clamp(t1, 0.0, 1.0), (1.0 - abs((1.0 - (t1 - (2.0 * floor((t1 / 2.0))))))), step(0.5, (input.v4).w)), (t1 - floor(t1)), step(1.5, (input.v4).w)) * 0.996094)), (input.v4).z)), step(0.5, input.v5));
    float2 t3 = ((input.v2 - (uniforms.u1).xy) * (uniforms.u1).zw);
    float2 t4 = abs((t3 - 0.5));
    return float4((t2).x, (t2).y, (t2).z, ((t2).w * (input.v1 * ((step((t4).x, 0.5) * step((t4).y, 0.5)) * (texture0.Sample(samplerConfig0, ((uniforms.u2).xy + (t3 * (uniforms.u2).zw)))).x))));
}
