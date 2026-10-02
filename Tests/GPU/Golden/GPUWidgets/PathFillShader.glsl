#version 450

layout(std140, set = 0, binding = 0) uniform Uniforms
{
    vec2 u0;
    vec4 u1;
} uniforms;

#ifdef EACP_VERTEX
layout(location = 0) in vec2 attr0;

void main()
{
    gl_Position = vec4((((attr0).x / ((uniforms.u0).x * 0.5)) - 1.0), (1.0 - ((attr0).y / ((uniforms.u0).y * 0.5))), 0.0, 1.0);
}
#endif

#ifdef EACP_FRAGMENT
layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = uniforms.u1;
}
#endif
