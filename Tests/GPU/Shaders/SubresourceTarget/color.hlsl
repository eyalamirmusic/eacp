struct VertexIn
{
    float3 position : TEXCOORD0;
    float4 color : TEXCOORD1;
};

struct VertexOut
{
    float4 position : SV_Position;
    float4 color : TEXCOORD0;
};

VertexOut vertexMain(VertexIn input)
{
    VertexOut o;
    o.position = float4(input.position, 1.0);
    o.color = input.color;
    return o;
}

float4 fragmentMain(VertexOut input) : SV_Target
{
    return input.color;
}
