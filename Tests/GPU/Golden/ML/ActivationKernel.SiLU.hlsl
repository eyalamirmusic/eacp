struct Uniforms
{
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
    float t0 = buffer0[gid];
    buffer1[gid] = (t0 * (1.0 / (1.0 + exp((-(t0))))));
}
