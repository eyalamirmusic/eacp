#ifndef EACP_OVERRIDE_0
#define EACP_OVERRIDE_0 1
#endif
RWStructuredBuffer<float> output : register(u0);

[numthreads(64, 1, 1)]
void computeMain(uint3 gid : SV_DispatchThreadID)
{
#if EACP_OVERRIDE_0 == 7
    output[gid.x] = brokenForSeven;
#else
    output[gid.x] = (float) EACP_OVERRIDE_0;
#endif
}
