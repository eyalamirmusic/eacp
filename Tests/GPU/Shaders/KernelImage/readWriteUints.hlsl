RWTexture2D<uint> image : register(u8);

[numthreads(8, 8, 1)]
void computeMain(uint3 gid : SV_DispatchThreadID)
{
    uint width, height;
    image.GetDimensions(width, height);

    if (gid.x >= width || gid.y >= height)
        return;

    image[gid.xy] = image[gid.xy] * 3u + 1u;
}
