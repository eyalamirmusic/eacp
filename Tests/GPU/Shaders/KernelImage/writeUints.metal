#include <metal_stdlib>
using namespace metal;

kernel void computeMain(texture2d<uint, access::write> target [[texture(0)]],
                        uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= target.get_width() || gid.y >= target.get_height())
        return;

    target.write(uint4((gid.x + gid.y * 4) * 1000003u + 7u, 0, 0, 0), gid);
}
