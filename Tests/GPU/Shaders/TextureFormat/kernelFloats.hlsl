RWTexture2D<float4> target : register(u8);

[numthreads(8, 8, 1)]
void computeMain(uint3 gid : SV_DispatchThreadID)
{
    uint width, height;
    target.GetDimensions(width, height);

    if (gid.x < width && gid.y < height)
        target[gid.xy] = float4(0.25, 0.5, 0.75, 1.0);
}
