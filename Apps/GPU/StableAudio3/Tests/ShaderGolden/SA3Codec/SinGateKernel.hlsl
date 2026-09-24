struct Uniforms
{
    uint u0;
    uint count;
};

cbuffer UniformsCB : register(b0)
{
    Uniforms uniforms;
};

StructuredBuffer<float> buffer0 : register(t0);
RWStructuredBuffer<float> buffer1 : register(u1);

[numthreads(64, 1, 1)]
void computeMain(uint3 threadId : SV_DispatchThreadID)
{
    uint gid = threadId.x;
    if (gid >= uniforms.count)
        return;
    uint t0 = (((gid / uniforms.u0) * uniforms.u0) * 2u);
    uint t1 = (gid % uniforms.u0);
    buffer1[gid] = (buffer0[(t0 + t1)] * sin((buffer0[((t0 + uniforms.u0) + t1)] * 3.14159)));
}
