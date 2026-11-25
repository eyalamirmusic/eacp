Texture2D<float> heights : register(t0);
SamplerState heightSampler : register(s8);

float4 vertexMain(float3 column : TEXCOORD0) : SV_Position
{
    float h = heights.SampleLevel(heightSampler, float2(column.y, 0.5), 0);
    return float4(column.x, -1.0 + 2.0 * h * column.z, 0.0, 1.0);
}

float4 fragmentMain(float4 position : SV_Position) : SV_Target
{
    return float4(1.0, 1.0, 1.0, 1.0);
}
