float4 vertexMain(float3 position : TEXCOORD0) : SV_Position
{
    return float4(position, 1.0);
}

float4 fragmentMain(float4 position : SV_Position) : SV_Target
{
    return float4(1.0, 0.0, 0.0, 1.0);
}
