#include <metal_stdlib>
using namespace metal;

kernel void computeMain(texture2d<float, access::write> target [[texture(0)]],
                        uint2 gid [[thread_position_in_grid]])
{
    if (gid.x < target.get_width() && gid.y < target.get_height())
        target.write(float4(0.25, 0.5, 0.75, 1.0), gid);
}
