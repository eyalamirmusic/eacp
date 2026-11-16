Texture2D<float> depth : register(t8);
RWStructuredBuffer<float> values : register(u0);

[numthreads(8, 8, 1)]
void computeMain(uint3 gid : SV_DispatchThreadID)
{
    uint width, height;
    depth.GetDimensions(width, height);

    if (gid.x >= width || gid.y >= height)
        return;

    values[gid.y * width + gid.x] = depth.Load(int3(gid.xy, 0));
}
