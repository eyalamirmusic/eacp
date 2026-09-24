struct Uniforms
{
    uint u0;
    uint u1;
    uint u2;
    uint u3;
    uint width;
    uint height;
};

cbuffer UniformsCB : register(b0)
{
    Uniforms uniforms;
};

StructuredBuffer<float> buffer0 : register(t0);
StructuredBuffer<float> buffer1 : register(t1);
RWStructuredBuffer<float> buffer2 : register(u2);

[numthreads(8, 8, 1)]
void computeMain(uint3 threadId : SV_DispatchThreadID)
{
    uint2 gid = threadId.xy;
    if (gid.x >= uniforms.width || gid.y >= uniforms.height)
        return;
    uint t0 = (gid.y % uniforms.u2);
    buffer2[((gid.y * uniforms.u0) + gid.x)] = ((t0 < uniforms.u1) ? buffer0[((min((((gid.y / uniforms.u2) * uniforms.u1) + t0), (uniforms.u3 - 1u)) * uniforms.u0) + gid.x)] : buffer1[gid.x]);
}
