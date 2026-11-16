#include <metal_stdlib>
using namespace metal;
#warning eacp sample warning
kernel void computeMain(device float* out [[buffer(0)]],
                        uint gid [[thread_position_in_grid]])
{
    out[gid] = missingValue;
}
