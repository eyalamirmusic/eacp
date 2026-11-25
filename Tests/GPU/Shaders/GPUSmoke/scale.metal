#include <metal_stdlib>
using namespace metal;

struct Params { float scale; uint count; };

kernel void computeMain(device const float* input [[buffer(0)]],
                        device float* output [[buffer(1)]],
                        constant Params& params [[buffer(16)]],
                        uint gid [[thread_position_in_grid]])
{
    if (gid >= params.count) return;
    output[gid] = input[gid] * params.scale;
}
