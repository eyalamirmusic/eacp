struct Uniforms
{
    uint u0;
    uint u1;
    uint u2;
    uint u3;
    uint u4;
    uint u5;
    uint u6;
    float u7;
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
    uint t0 = (gid / uniforms.u2);
    uint t1 = (t0 / uniforms.u0);
    uint t2 = ((t1 / uniforms.u4) * uniforms.u4);
    uint t3 = ((max(t1, (t2 + uniforms.u5)) - uniforms.u5) + (gid % uniforms.u2));
    if ((t3 < min(min((t2 + uniforms.u4), uniforms.u3), ((t1 + uniforms.u6) + 1u))))
    {
        float v0 = 0.0;
        uint v1 = 0u;
        while ((v1 < uniforms.u1))
        {
            v0 = (v0 + (buffer0[((t0 * uniforms.u1) + v1)] * buffer1[((((t3 * uniforms.u0) + (t0 % uniforms.u0)) * uniforms.u1) + v1)]));
            v1 = (v1 + 1u);
        }
        buffer2[gid] = (v0 * uniforms.u7);
    }
}
