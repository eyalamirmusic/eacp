struct Uniforms
{
    uint u0;
    uint u1;
    uint count;
};

cbuffer UniformsCB : register(b0)
{
    Uniforms uniforms;
};

RWStructuredBuffer<uint> buffer0 : register(u0);
RWStructuredBuffer<uint> buffer1 : register(u1);

[numthreads(64, 1, 1)]
void computeMain(uint3 threadId : SV_DispatchThreadID)
{
    uint gid = threadId.x;
    if (gid >= uniforms.count)
        return;
    if ((gid < uniforms.u0))
    {
        buffer0[gid] = 0u;
    }
    if ((gid < uniforms.u1))
    {
        buffer1[gid] = 0u;
    }
}
