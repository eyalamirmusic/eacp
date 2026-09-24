struct Uniforms
{
    float u0;
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
    buffer0[gid] = uniforms.u0;
}
