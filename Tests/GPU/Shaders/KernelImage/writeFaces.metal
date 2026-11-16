#include <metal_stdlib>
using namespace metal;

kernel void computeMain(texture2d_array<float, access::write> faces [[texture(0)]],
                        uint3 gid [[thread_position_in_grid]])
{
    if (gid.x >= faces.get_width() || gid.y >= faces.get_height()
        || gid.z >= faces.get_array_size())
        return;

    float face = float(gid.z);
    faces.write(float4((40.0 * face + 20.0) / 255.0, (200.0 - 30.0 * face) / 255.0,
                       64.0 / 255.0, 1.0),
                gid.xy, gid.z);
}
