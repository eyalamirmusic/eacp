float4 vertexMain(float2 position : TEXCOORD0) : SV_Position
{
    return float4(position, 0.0, 1.0);
}

float4 fragmentMain(float4 position : SV_Position) : SV_Target
{
    return float4(0.25, 0.5, 0.75, 1.0);
}
