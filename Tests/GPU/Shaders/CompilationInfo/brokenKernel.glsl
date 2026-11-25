#version 450
layout(local_size_x = 64) in;
layout(std430, binding = 0) buffer Out { float values[]; } outBuffer;

void main()
{
    outBuffer.values[gl_GlobalInvocationID.x] = undeclaredThing * 2.0;
}
