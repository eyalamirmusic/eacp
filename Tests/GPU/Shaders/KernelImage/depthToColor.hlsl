Texture2DArray<float> depth : register(t0);

float4 vertexMain(float2 position : TEXCOORD0) : SV_Position
{
    return float4(position, 0.5, 1.0);
}

float4 fragmentMain(float4 position : SV_Position) : SV_Target
{
    return float4(depth.Load(int4(position.xy, 0, 0)), 0.0, 0.0, 1.0);
}
