#include <metal_stdlib>
using namespace metal;

kernel void computeMain(texture2d<float, access::read> source [[texture(0)]],
                        texture2d<float, access::write> target [[texture(1)]],
                        uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= target.get_width() || gid.y >= target.get_height())
        return;

    uint2 at = gid * 2;
    float4 sum = source.read(at) + source.read(at + uint2(1, 0))
                 + source.read(at + uint2(0, 1)) + source.read(at + uint2(1, 1));
    target.write(sum * 0.25, gid);
}
