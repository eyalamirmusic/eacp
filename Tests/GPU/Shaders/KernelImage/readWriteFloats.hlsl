RWTexture2D<float4> image : register(u8);

[numthreads(8, 8, 1)]
void computeMain(uint3 gid : SV_DispatchThreadID)
{
    uint width, height;
    image.GetDimensions(width, height);

    if (gid.x >= width || gid.y >= height)
        return;

    float4 value = image[gid.xy];
    image[gid.xy] = float4(1.0 - value.x, value.yzw);
}
