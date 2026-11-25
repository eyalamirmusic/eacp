TextureCube<float4> cube : register(t0);
SamplerState cubeSampler : register(s8);

static const float3 directions[6] = {
    float3(1.0, 0.0, 0.0), float3(-1.0, 0.0, 0.0),
    float3(0.0, 1.0, 0.0), float3(0.0, -1.0, 0.0),
    float3(0.0, 0.0, 1.0), float3(0.0, 0.0, -1.0)};

float4 vertexMain(float2 position : TEXCOORD0) : SV_Position
{
    return float4(position, 0.0, 1.0);
}

float4 fragmentMain(float4 position : SV_Position) : SV_Target
{
    return cube.Sample(cubeSampler, directions[uint(position.x)]);
}
