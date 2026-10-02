struct Uniforms
{
    uint u0;
    uint u1;
    uint u2;
    uint u3;
    uint u4;
    uint u5;
    uint u6;
    uint u7;
    uint u8;
    uint count;
};

cbuffer UniformsCB : register(b0)
{
    Uniforms uniforms;
};

StructuredBuffer<float> buffer0 : register(t0);
StructuredBuffer<float> buffer1 : register(t1);
StructuredBuffer<float> buffer2 : register(t2);
RWStructuredBuffer<float> buffer3 : register(u3);

[numthreads(64, 1, 1)]
void computeMain(uint3 threadId : SV_DispatchThreadID)
{
    uint gid = threadId.x;
    if (gid >= uniforms.count)
        return;
    float v0 = 0.0;
    uint t0 = (gid / uniforms.u1);
    uint t1 = (t0 / uniforms.u0);
    uint t2 = (max(t1, (((t1 / uniforms.u4) * uniforms.u4) + uniforms.u5)) - uniforms.u5);
    uint v1 = t2;
    while ((v1 < min(min((((t1 / uniforms.u4) * uniforms.u4) + uniforms.u4), uniforms.u3), ((t1 + uniforms.u6) + 1u))))
    {
        v0 = (v0 + (buffer1[(((t0 * uniforms.u2) - t2) + v1)] * buffer0[((((v1 * uniforms.u7) + uniforms.u8) + ((t0 % uniforms.u0) * uniforms.u1)) + (gid % uniforms.u1))]));
        v1 = (v1 + 1u);
    }
    uint t3 = (gid % uniforms.u1);
    buffer3[((t0 * uniforms.u1) + t3)] = (v0 / buffer2[t0]);
}
