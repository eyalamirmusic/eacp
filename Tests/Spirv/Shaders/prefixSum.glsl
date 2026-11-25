#version 450

layout(local_size_x = 64) in;

layout(std140, set = 0, binding = 0) uniform Uniforms
{
    uint elementCount;
} uniforms;

layout(std430, set = 0, binding = 24) buffer Values
{
    float values[];
} valuesBuffer;

void main()
{
    uint index = gl_GlobalInvocationID.x;

    if (index < uniforms.elementCount)
        valuesBuffer.values[index] = valuesBuffer.values[index] * 2.0;
}
