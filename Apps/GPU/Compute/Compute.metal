#include <metal_stdlib>
using namespace metal;

// Per-dispatch constants. Mirrors the C++ Params struct uploaded with
// ComputePass::setUniform; bound at the uniform base (buffer 16).
struct Params
{
    float scale;
    uint count;
};

// out[i] = in[i] * scale. The grid is rounded up to whole threadgroups, so
// threads past count must do nothing.
kernel void computeMain(device const float* input [[buffer(0)]],
                        device float* output [[buffer(1)]],
                        constant Params& params [[buffer(16)]],
                        uint gid [[thread_position_in_grid]])
{
    if (gid >= params.count)
        return;

    output[gid] = input[gid] * params.scale;
}
