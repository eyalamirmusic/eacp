struct VertexIn
{
    float2 a0 : TEXCOORD0;
};

struct VertexOut
{
    float4 position : SV_Position;
};

struct Uniforms
{
    float2 u0;
    float4 u1;
};

cbuffer UniformsCB : register(b0)
{
    Uniforms uniforms;
};

VertexOut vertexMain(VertexIn input)
{
    VertexOut output;
    output.position = float4((((input.a0).x / ((uniforms.u0).x * 0.5)) - 1.0), (1.0 - ((input.a0).y / ((uniforms.u0).y * 0.5))), 0.0, 1.0);
    return output;
}

float4 fragmentMain(VertexOut input) : SV_Target
{
    return uniforms.u1;
}
