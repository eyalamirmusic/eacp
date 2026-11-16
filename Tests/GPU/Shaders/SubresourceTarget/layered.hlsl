struct VertexIn
{
    float3 position : TEXCOORD0;
    float4 color : TEXCOORD1;
    uint instance : SV_InstanceID;
};

struct VertexOut
{
    float4 position : SV_Position;
    float4 color : TEXCOORD0;
    uint layer : SV_RenderTargetArrayIndex;
};

VertexOut vertexMain(VertexIn input)
{
    VertexOut o;
    o.position = float4(input.position, 1.0);
    o.color = float4(float(input.instance + 1) * 0.2, 0.0, 0.0, 1.0);
    o.layer = input.instance;
    return o;
}

float4 fragmentMain(VertexOut input) : SV_Target
{
    return input.color;
}
