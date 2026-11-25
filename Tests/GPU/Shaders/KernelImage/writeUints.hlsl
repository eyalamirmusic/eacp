RWTexture2D<uint> target : register(u8);

[numthreads(8, 8, 1)]
void computeMain(uint3 gid : SV_DispatchThreadID)
{
    uint width, height;
    target.GetDimensions(width, height);

    if (gid.x >= width || gid.y >= height)
        return;

    target[gid.xy] = (gid.x + gid.y * 4) * 1000003u + 7u;
}
