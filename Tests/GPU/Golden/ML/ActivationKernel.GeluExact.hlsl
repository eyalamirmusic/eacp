float eacpErf(float x)
{
    float a = abs(x);
    float t = 1.0 / (1.0 + 0.3275911 * a);
    float e = 1.0 - t * (0.254829592 + t * (-0.284496736 + t * (1.421413741
              + t * (-1.453152027 + t * 1.061405429)))) * exp(-a * a);
    return a == 0.0 ? x : (x < 0.0 ? -e : e);
}

float2 eacpErf(float2 x)
{
    return float2(eacpErf(x.x), eacpErf(x.y));
}

float3 eacpErf(float3 x)
{
    return float3(eacpErf(x.x), eacpErf(x.y), eacpErf(x.z));
}

float4 eacpErf(float4 x)
{
    return float4(eacpErf(x.x), eacpErf(x.y), eacpErf(x.z), eacpErf(x.w));
}

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
    buffer1[gid] = ((0.5 * t0) * (1.0 + eacpErf((t0 * 0.707107))));
}
