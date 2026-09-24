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

StructuredBuffer<float> buffer0 : register(t0);
StructuredBuffer<float> buffer1 : register(t1);
RWStructuredBuffer<float> buffer2 : register(u2);

[numthreads(64, 1, 1)]
void computeMain(uint3 threadId : SV_DispatchThreadID)
{
    uint gid = threadId.x;
    if (gid >= uniforms.count)
        return;
    buffer2[gid] = (((gid / uniforms.u0) < uniforms.u1) ? buffer0[gid] : buffer1[(gid % uniforms.u0)]);
}
