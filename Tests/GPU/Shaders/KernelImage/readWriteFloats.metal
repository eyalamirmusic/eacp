#include <metal_stdlib>
using namespace metal;

kernel void computeMain(texture2d<float, access::read_write> image [[texture(0)]],
                        uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= image.get_width() || gid.y >= image.get_height())
        return;

    float4 value = image.read(gid);
    image.write(float4(1.0 - value.x, value.yzw), gid);
}
