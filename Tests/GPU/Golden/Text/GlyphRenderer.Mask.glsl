#version 450

layout(std140, set = 0, binding = 0) uniform Uniforms
{
    vec2 u0;
    vec2 u1;
} uniforms;

layout(set = 0, binding = 8) uniform sampler2D texture0;

#ifdef EACP_VERTEX
layout(location = 0) in vec2 attr0;
layout(location = 1) in vec4 attr1;
layout(location = 2) in vec4 attr2;
layout(location = 3) in vec4 attr3;
layout(location = 0) out vec2 vary0;
layout(location = 1) out vec4 vary1;

void main()
{
    vec2 t0 = vec2(((attr1).x + ((attr0).x * (attr1).z)), ((attr1).y + ((attr0).y * (attr1).w)));
    gl_Position = vec4(((((t0).x / (uniforms.u0).x) * 2.0) - 1.0), (1.0 - (((t0).y / (uniforms.u0).y) * 2.0)), 0.0, 1.0);
    vary0 = vec2((((attr2).x + ((attr0).x * (attr2).z)) / (uniforms.u1).x), (((attr2).y + ((attr0).y * (attr2).w)) / (uniforms.u1).y));
    vary1 = attr3;
}
#endif

#ifdef EACP_FRAGMENT
layout(location = 0) in vec2 vary0;
layout(location = 1) in vec4 vary1;
layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = vec4((vary1).x, (vary1).y, (vary1).z, ((vary1).w * (texture(texture0, vary0)).x));
}
#endif
