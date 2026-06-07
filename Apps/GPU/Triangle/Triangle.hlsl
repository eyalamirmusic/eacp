// Vertex inputs are bound by TEXCOORD semantic index, mirroring the order of the
// attributes in the RenderPipelineDescriptor's VertexLayout (the D3D11 backend
// maps attribute i to TEXCOORD<i>).
struct VertexIn
{
    float2 position : TEXCOORD0;
    float3 color : TEXCOORD1;
};

struct VertexOut
{
    float4 position : SV_Position;
    float3 color : COLOR0;
};

VertexOut vertexMain(VertexIn input)
{
    VertexOut output;
    output.position = float4(input.position, 0.0, 1.0);
    output.color = input.color;
    return output;
}

float4 fragmentMain(VertexOut input) : SV_Target
{
    return float4(input.color, 1.0);
}
