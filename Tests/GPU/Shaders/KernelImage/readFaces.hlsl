Texture2DArray<float4> faces : register(t8);
RWStructuredBuffer<float4> values : register(u0);

[numthreads(64, 1, 1)]
void computeMain(uint3 gid : SV_DispatchThreadID)
{
    uint width, height, count;
    faces.GetDimensions(width, height, count);

    if (gid.x >= count)
        return;

    values[gid.x] = faces.Load(int4(1, 1, gid.x, 0));
}
