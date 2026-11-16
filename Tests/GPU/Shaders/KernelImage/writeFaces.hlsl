RWTexture2DArray<float4> faces : register(u8);

[numthreads(4, 4, 4)]
void computeMain(uint3 gid : SV_DispatchThreadID)
{
    uint width, height, count;
    faces.GetDimensions(width, height, count);

    if (gid.x >= width || gid.y >= height || gid.z >= count)
        return;

    float face = float(gid.z);
    faces[gid] = float4((40.0 * face + 20.0) / 255.0, (200.0 - 30.0 * face) / 255.0,
                        64.0 / 255.0, 1.0);
}
