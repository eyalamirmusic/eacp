#version 450

layout(std140, set = 0, binding = 0) uniform Uniforms
{
    vec2 u0;
    vec4 u1;
    vec4 u2;
} uniforms;

layout(set = 0, binding = 8) uniform sampler2D texture0;

#ifdef EACP_VERTEX
layout(location = 0) in vec2 attr0;
layout(location = 0) out vec2 vary0;

void main()
{
    gl_Position = vec4(((((uniforms.u1).x + ((attr0).x * (uniforms.u1).z)) / ((uniforms.u0).x * 0.5)) - 1.0), (1.0 - (((uniforms.u1).y + ((attr0).y * (uniforms.u1).w)) / ((uniforms.u0).y * 0.5))), 0.0, 1.0);
    vary0 = attr0;
}
#endif

#ifdef EACP_FRAGMENT
layout(location = 0) in vec2 vary0;
layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = vec4((uniforms.u2).xyz, ((uniforms.u2).w * (texture(texture0, vary0)).x));
}
#endif
