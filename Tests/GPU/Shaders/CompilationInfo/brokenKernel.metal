#include <metal_stdlib>
using namespace metal;

kernel void computeMain(device float* out [[buffer(0)]],
                        uint gid [[thread_position_in_grid]])
{
    out[gid] = undeclaredThing * 2.0f;
}
