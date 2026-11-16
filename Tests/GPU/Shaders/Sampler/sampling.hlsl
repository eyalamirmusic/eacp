Texture2D image : register(t0);
SamplerState imageSampler : register(s8);

struct VertexIn
{
    float2 position : TEXCOORD0;
    float2 uv : TEXCOORD1;
};

struct VertexOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOut vertexMain(VertexIn input)
{
    VertexOut o;
    o.position = float4(input.position, 0.0, 1.0);
    o.uv = input.uv;
    return o;
}

float4 fragmentMain(VertexOut input) : SV_Target
{
    return image.Sample(imageSampler, input.uv);
}
