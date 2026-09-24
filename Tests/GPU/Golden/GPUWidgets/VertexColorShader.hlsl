struct VertexIn
{
    float2 a0 : TEXCOORD0;
    float4 a1 : TEXCOORD1;
};

struct VertexOut
{
    float4 position : SV_Position;
    float4 v0 : TEXCOORD0;
};

struct Uniforms
{
    float2 u0;
};

cbuffer UniformsCB : register(b0)
{
    Uniforms uniforms;
};

VertexOut vertexMain(VertexIn input)
{
    VertexOut output;
    output.position = float4((((input.a0).x / ((uniforms.u0).x * 0.5)) - 1.0), (1.0 - ((input.a0).y / ((uniforms.u0).y * 0.5))), 0.0, 1.0);
    output.v0 = input.a1;
    return output;
}

float4 fragmentMain(VertexOut input) : SV_Target
{
    return input.v0;
}
