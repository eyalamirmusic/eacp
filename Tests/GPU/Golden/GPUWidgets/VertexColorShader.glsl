#version 450

layout(std140, set = 0, binding = 0) uniform Uniforms
{
    vec2 u0;
} uniforms;

#ifdef EACP_VERTEX
layout(location = 0) in vec2 attr0;
layout(location = 1) in vec4 attr1;
layout(location = 0) out vec4 vary0;

void main()
{
    gl_Position = vec4((((attr0).x / ((uniforms.u0).x * 0.5)) - 1.0), (1.0 - ((attr0).y / ((uniforms.u0).y * 0.5))), 0.0, 1.0);
    vary0 = attr1;
}
#endif

#ifdef EACP_FRAGMENT
layout(location = 0) in vec4 vary0;
layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = vary0;
}
#endif
