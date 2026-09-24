struct Uniforms
{
    float u0;
    float u1;
    float u2;
    float u3;
    uint u4;
    uint count;
};

cbuffer UniformsCB : register(b0)
{
    Uniforms uniforms;
};

RWStructuredBuffer<float> buffer0 : register(u0);

[numthreads(64, 1, 1)]
void computeMain(uint3 threadId : SV_DispatchThreadID)
{
    uint gid = threadId.x;
    if (gid >= uniforms.count)
        return;
    float t0 = ((uniforms.u0 * exp((((float(gid) / uniforms.u3) * (uniforms.u2 - uniforms.u1)) + uniforms.u1))) * 6.28319);
    buffer0[gid] = cos(t0);
    buffer0[(uniforms.u4 + gid)] = sin(t0);
}
