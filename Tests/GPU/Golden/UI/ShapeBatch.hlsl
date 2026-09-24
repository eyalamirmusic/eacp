struct VertexIn
{
    float2 a0 : TEXCOORD0;
    float2 a1 : TEXCOORD1;
    float2 a2 : TEXCOORD2;
    float2 a3 : TEXCOORD3;
    float2 a4 : TEXCOORD4;
    float2 a5 : TEXCOORD5;
    float4 a6 : TEXCOORD6;
    float4 a7 : TEXCOORD7;
    float4 a8 : TEXCOORD8;
    float4 a9 : TEXCOORD9;
    float4 a10 : TEXCOORD10;
    float2 a11 : TEXCOORD11;
    float4 a12 : TEXCOORD12;
};

struct VertexOut
{
    float4 position : SV_Position;
    float2 v0 : TEXCOORD0;
    float2 v1 : TEXCOORD1;
    float4 v2 : TEXCOORD2;
    float4 v3 : TEXCOORD3;
    float2 v4 : TEXCOORD4;
    float4 v5 : TEXCOORD5;
    float4 v6 : TEXCOORD6;
    float2 v7 : TEXCOORD7;
    float4 v8 : TEXCOORD8;
    float2 v9 : TEXCOORD9;
};

struct Uniforms
{
    float2 u0;
    float u1;
    float4 u2;
    float4 u3;
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
    output.v0 = ((input.a0 - 0.5) * (input.a5 * 2.0));
    output.v1 = input.a4;
    output.v2 = input.a6;
    output.v3 = input.a7;
    output.v4 = t0;
    output.v5 = input.a9;
    output.v6 = input.a10;
    output.v7 = input.a11;
    output.v8 = input.a12;
    output.v9 = ((input.a8).xy + (input.a0 * (input.a8).zw));
    return output;
}

float4 fragmentMain(VertexOut input) : SV_Target
{
    float t0 = ((((input.v5).x * (input.v4).x) + ((input.v5).y * (input.v4).y)) + (input.v6).x);
    float t1 = lerp(t0, length(float2(t0, ((((input.v5).z * (input.v4).x) + ((input.v5).w * (input.v4).y)) + (input.v6).y))), step(1.5, (input.v3).w));
    float4 t2 = lerp(input.v2, texture1.Sample(samplerConfig2, float2((0.00195312 + (lerp(lerp(clamp(t1, 0.0, 1.0), (1.0 - abs((1.0 - (t1 - (2.0 * floor((t1 / 2.0))))))), step(0.5, (input.v6).w)), (t1 - floor(t1)), step(1.5, (input.v6).w)) * 0.996094)), (input.v6).z)), step(0.5, (input.v3).w));
    float2 t3 = (abs(input.v0) - (input.v1 - float2((input.v3).x, (input.v3).x)));
    float t4 = ((length(max(0.0, t3)) + min(0.0, max((t3).x, (t3).y))) - (input.v3).x);
    float t5 = ((input.v3).y * 0.5);
    float t6 = lerp(t4, (abs((t4 + t5)) - t5), (input.v3).z);
    float t7 = clamp((0.5 - (t6 * uniforms.u1)), 0.0, 1.0);
    float t8 = lerp(t7, min(smoothstep(0.0, 1.0, clamp((0.5 - (t6 * (input.v7).x)), 0.0, 1.0)), (smoothstep(0.0, 1.0, clamp((0.5 - ((abs((input.v0).x) - (input.v1).x) * (input.v7).x)), 0.0, 1.0)) * smoothstep(0.0, 1.0, clamp((0.5 - ((abs((input.v0).y) - (input.v1).y) * (input.v7).x)), 0.0, 1.0)))), step(0.0001, (input.v7).x));
    float t9 = (max(0.0, ((input.v3).x + (input.v8).z)) * step(0.001, (input.v3).x));
    float2 t10 = (abs((input.v0 - (input.v8).xy)) - ((input.v1 + float2((input.v8).z, (input.v8).z)) - float2(t9, t9)));
    float t11 = clamp((0.5 - (((length(max(0.0, t10)) + min(0.0, max((t10).x, (t10).y))) - t9) * uniforms.u1)), 0.0, 1.0);
    float2 t12 = ((input.v4 - (uniforms.u2).xy) * (uniforms.u2).zw);
    float2 t13 = abs((t12 - 0.5));
    return float4((t2).x, (t2).y, (t2).z, ((t2).w * ((lerp(t7, lerp((t8 * (1.0 - t11)), ((1.0 - t8) * t11), (input.v8).w), (input.v7).y) * (texture0.Sample(samplerConfig0, input.v9)).x) * ((step((t13).x, 0.5) * step((t13).y, 0.5)) * (texture0.Sample(samplerConfig0, ((uniforms.u3).xy + (t12 * (uniforms.u3).zw)))).x))));
}
