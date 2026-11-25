RWTexture2D<float4> source : register(u8);
RWTexture2D<float4> target : register(u9);

[numthreads(8, 8, 1)]
void computeMain(uint3 gid : SV_DispatchThreadID)
{
    uint width, height;
    target.GetDimensions(width, height);

    if (gid.x >= width || gid.y >= height)
        return;

    uint2 at = gid.xy * 2;
    float4 sum = source[at] + source[at + uint2(1, 0)] + source[at + uint2(0, 1)]
                 + source[at + uint2(1, 1)];
    target[gid.xy] = sum * 0.25;
}
