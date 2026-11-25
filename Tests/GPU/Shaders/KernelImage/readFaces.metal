#include <metal_stdlib>
using namespace metal;

kernel void computeMain(texture2d_array<float, access::read> faces [[texture(0)]],
                        device float4* values [[buffer(0)]],
                        uint gid [[thread_position_in_grid]])
{
    if (gid >= faces.get_array_size())
        return;

    values[gid] = faces.read(uint2(1, 1), gid);
}
