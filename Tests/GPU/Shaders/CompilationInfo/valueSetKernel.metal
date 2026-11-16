#include <metal_stdlib>
using namespace metal;
constant int eacpOverride0 [[function_constant(0)]];
kernel void computeMain(device float* out [[buffer(0)]],
                        uint gid [[thread_position_in_grid]])
{
    out[gid] = 1.0f;
}
