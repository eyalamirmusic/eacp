struct Uniforms
{
    int u0;
    uint count;
};

cbuffer UniformsCB : register(b0)
{
    Uniforms uniforms;
};

StructuredBuffer<float> buffer0 : register(t0);
RWStructuredBuffer<uint> buffer1 : register(u1);
StructuredBuffer<float> buffer2 : register(t2);

[numthreads(64, 1, 1)]
void computeMain(uint3 threadId : SV_DispatchThreadID)
{
    uint gid = threadId.x;
    if (gid >= uniforms.count)
        return;
    int v0 = 0;
    int v1 = uniforms.u0;
    while ((v0 < v1))
    {
        int t0 = ((v0 + v1) >> 1);
        if ((buffer2[uint(t0)] <= float(gid)))
        {
            v0 = (t0 + 1);
        }
        else
        {
            v1 = t0;
        }
    }
    uint t1 = uint((max(v0, 1) - 1));
    uint t2 = ((t1 * 2u) * 4u);
    float4 t3 = float4(buffer0[t2], buffer0[t2 + 1u], buffer0[t2 + 2u], buffer0[t2 + 3u]);
    uint v2 = uint((t3).x);
    uint v3 = uint((t3).z);
    uint v4 = ((uint((t3).y) + 15u) / 16u);
    uint v5 = (gid - uint(buffer2[t1]));
    uint v6 = 0u;
    uint v7 = 0u;
    while ((v7 < v4))
    {
        uint t4 = ((v2 + (v7 * v3)) + v5);
        v6 = (v6 + buffer1[t4]);
        buffer1[t4] = v6;
        v7 = (v7 + 1u);
    }
}
