#version 450
layout(local_size_x = 64) in;
layout(constant_id = 0) const int override_k = 1;
layout(std430, binding = 0) buffer Out { float values[]; } outBuffer;

void main()
{
    outBuffer.values[gl_GlobalInvocationID.x] = float(override_k);
}
