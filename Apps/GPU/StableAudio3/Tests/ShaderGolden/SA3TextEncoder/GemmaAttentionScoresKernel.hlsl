struct Uniforms
{
    uint u0;
    uint u1;
    uint u2;
    float u3;
    float u4;
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
    uint v1 = 0u;
    while ((v1 < uniforms.u1))
    {
        uint t0 = (gid / uniforms.u2);
        v0 = (v0 + (buffer0[((t0 * uniforms.u1) + v1)] * buffer1[(((((gid % uniforms.u2) * uniforms.u0) + (t0 % uniforms.u0)) * uniforms.u1) + v1)]));
        v1 = (v1 + 1u);
    }
    uint t1 = (gid / uniforms.u2);
    uint t2 = (gid % uniforms.u2);
    buffer3[gid] = ((uniforms.u4 * tanh(((v0 * uniforms.u3) / uniforms.u4))) + buffer2[(((t1 / uniforms.u0) * uniforms.u2) + t2)]);
}
