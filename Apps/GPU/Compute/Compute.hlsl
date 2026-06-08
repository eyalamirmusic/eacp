// Compute I/O uses views: the read-only input is a shader-resource view (t0,
// ComputePass slot 0) and the read-write output an unordered-access view (u1,
// slot 1). The Metal kernel binds the same two buffers in one flat index space;
// the register numbers here match the C++ bind slots.
StructuredBuffer<float> input : register(t0);
RWStructuredBuffer<float> output : register(u1);

cbuffer Params : register(b0)
{
    float scale;
    uint count;
};

// out[i] = in[i] * scale. numthreads matches ComputePass::threadGroupWidth, and
// the dispatch rounds up to whole groups, so guard against the overrun.
[numthreads(64, 1, 1)]
void computeMain(uint3 gid : SV_DispatchThreadID)
{
    if (gid.x >= count)
        return;

    output[gid.x] = input[gid.x] * scale;
}
