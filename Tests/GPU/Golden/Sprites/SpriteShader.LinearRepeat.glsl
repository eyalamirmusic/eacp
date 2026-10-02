#version 450

layout(std140, set = 0, binding = 0) uniform Uniforms
{
    vec2 u0;
} uniforms;

layout(set = 0, binding = 8) uniform sampler2D texture0;

#ifdef EACP_VERTEX
layout(location = 0) in vec2 attr0;
layout(location = 1) in vec2 attr1;
layout(location = 2) in vec2 attr2;
layout(location = 3) in vec2 attr3;
layout(location = 4) in vec2 attr4;
layout(location = 5) in vec2 attr5;
layout(location = 6) in vec4 attr6;
layout(location = 0) out vec2 vary0;
layout(location = 1) out vec4 vary1;

void main()
{
    vec2 t0 = ((attr1 + ((attr0).x * attr2)) + ((attr0).y * attr3));
    gl_Position = vec4(((((t0).x / (uniforms.u0).x) * 2.0) - 1.0), (1.0 - (((t0).y / (uniforms.u0).y) * 2.0)), 0.0, 1.0);
    vary0 = (attr4 + (attr0 * (attr5 - attr4)));
    vary1 = attr6;
}
#endif

#ifdef EACP_FRAGMENT
layout(location = 0) in vec2 vary0;
layout(location = 1) in vec4 vary1;
layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = (texture(texture0, vary0) * vary1);
}
#endif
