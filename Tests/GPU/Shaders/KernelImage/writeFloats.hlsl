RWTexture2D<float4> target : register(u8);

[numthreads(8, 8, 1)]
void computeMain(uint3 gid : SV_DispatchThreadID)
{
    uint width, height;
    target.GetDimensions(width, height);

    if (gid.x >= width || gid.y >= height)
        return;

    float a = float(gid.x + gid.y * 4 + 1) / 255.0;
    float b = float(gid.x * 3 + 2) / 255.0;
    target[gid.xy] = float4(a, b, 0.5, 1.0);
}
