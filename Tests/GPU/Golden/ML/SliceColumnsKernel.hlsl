struct Uniforms
{
    uint u0;
    uint u1;
    uint u2;
    uint width;
    uint height;
};

cbuffer UniformsCB : register(b0)
{
    Uniforms uniforms;
};

StructuredBuffer<float> buffer0 : register(t0);
RWStructuredBuffer<float> buffer1 : register(u1);

[numthreads(8, 8, 1)]
void computeMain(uint3 threadId : SV_DispatchThreadID)
{
    uint2 gid = threadId.xy;
    if (gid.x >= uniforms.width || gid.y >= uniforms.height)
        return;
    buffer1[((gid.y * uniforms.u2) + gid.x)] = buffer0[(((gid.y * uniforms.u0) + uniforms.u1) + gid.x)];
}
