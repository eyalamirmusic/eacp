float4 vertexMain(float2 position : TEXCOORD0) : SV_Position
{
    return float4(position, 0.25, 1.0);
}
