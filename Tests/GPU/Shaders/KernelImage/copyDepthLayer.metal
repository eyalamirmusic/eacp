#include <metal_stdlib>
using namespace metal;

kernel void computeMain(depth2d_array<float, access::read> depth [[texture(0)]],
                        device float* values [[buffer(0)]],
                        constant uint& layer [[buffer(16)]],
                        uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= depth.get_width() || gid.y >= depth.get_height())
        return;

    values[gid.y * depth.get_width() + gid.x] = depth.read(gid, layer);
}
