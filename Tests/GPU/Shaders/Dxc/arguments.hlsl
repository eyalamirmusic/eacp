RWStructuredBuffer<uint> output : register(u0);
[numthreads(64, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
#ifdef EACP_DXC_BREAK
    undeclared[id.x] = 1;
#endif
    output[id.x] = id.x;
}
