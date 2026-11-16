#include <metal_stdlib>
using namespace metal;

kernel void computeMain(texture2d<uint, access::read_write> image [[texture(0)]],
                        uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= image.get_width() || gid.y >= image.get_height())
        return;

    uint value = image.read(gid).x;
    image.write(uint4(value * 3u + 1u, 0, 0, 0), gid);
}
