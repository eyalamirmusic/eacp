#include <metal_stdlib>
using namespace metal;

kernel void computeMain(texture2d<float, access::write> target [[texture(0)]],
                        uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= target.get_width() || gid.y >= target.get_height())
        return;

    float a = float(gid.x + gid.y * 4 + 1) / 255.0;
    float b = float(gid.x * 3 + 2) / 255.0;
    target.write(float4(a, b, 0.5, 1.0), gid);
}
