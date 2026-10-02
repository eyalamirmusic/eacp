float eacpSaturatingTanh(float x)
{
    return x >= 10.0 ? 1.0 : (x <= -10.0 ? -1.0 : tanh(x));
}

float2 eacpSaturatingTanh(float2 x)
{
    return float2(eacpSaturatingTanh(x.x), eacpSaturatingTanh(x.y));
}

float3 eacpSaturatingTanh(float3 x)
{
    return float3(eacpSaturatingTanh(x.x), eacpSaturatingTanh(x.y),
                  eacpSaturatingTanh(x.z));
}

float4 eacpSaturatingTanh(float4 x)
{
    return float4(eacpSaturatingTanh(x.x), eacpSaturatingTanh(x.y),
                  eacpSaturatingTanh(x.z), eacpSaturatingTanh(x.w));
}

struct Uniforms
{
    uint u0;
    float u1;
    uint u2;
    uint u3;
    uint u4;
    uint width;
    uint height;
};

cbuffer UniformsCB : register(b0)
{
    Uniforms uniforms;
};

StructuredBuffer<float> buffer0 : register(t0);
StructuredBuffer<float> buffer1 : register(t1);
StructuredBuffer<float> buffer2 : register(t2);
RWStructuredBuffer<float> buffer3 : register(u3);

[numthreads(8, 8, 1)]
void computeMain(uint3 threadId : SV_DispatchThreadID)
{
    uint2 gid = threadId.xy;
    if (gid.x >= uniforms.width || gid.y >= uniforms.height)
        return;
    buffer3[((gid.y * uniforms.u0) + gid.x)] = ((eacpSaturatingTanh((uniforms.u1 * buffer0[(((((gid.y / uniforms.u2) * uniforms.u3) + uniforms.u4) + ((gid.y % uniforms.u2) * uniforms.u0)) + gid.x)])) * buffer1[gid.x]) + buffer2[gid.x]);
}
