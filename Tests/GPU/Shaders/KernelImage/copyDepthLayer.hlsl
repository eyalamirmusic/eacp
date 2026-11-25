Texture2DArray<float> depth : register(t8);
RWStructuredBuffer<float> values : register(u0);

cbuffer Params : register(b0)
{
    uint layer;
};

[numthreads(8, 8, 1)]
void computeMain(uint3 gid : SV_DispatchThreadID)
{
    uint width, height, layers;
    depth.GetDimensions(width, height, layers);

    if (gid.x >= width || gid.y >= height)
        return;

    values[gid.y * width + gid.x] = depth.Load(int4(gid.xy, layer, 0));
}
