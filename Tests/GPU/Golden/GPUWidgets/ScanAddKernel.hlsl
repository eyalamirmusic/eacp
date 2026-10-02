struct Uniforms
{
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
    buffer0[gid] = (buffer0[gid] + buffer1[(gid / 1024u)]);
}
