#include <metal_stdlib>
using namespace metal;

struct Params { float scale; uint count; };

kernel void computeMain(device const float* low [[buffer(7)]],
                        device const float* high [[buffer(12)]],
                        device float* result [[buffer(15)]],
                        constant Params& params [[buffer(16)]],
                        uint gid [[thread_position_in_grid]])
{
    if (gid >= params.count) return;
    result[gid] = low[gid] + high[gid] * params.scale;
}
