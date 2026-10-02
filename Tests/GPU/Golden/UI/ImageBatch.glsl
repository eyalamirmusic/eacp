#version 450

layout(std140, set = 0, binding = 0) uniform Uniforms
{
    vec2 u0;
    vec4 u1;
    vec4 u2;
} uniforms;

layout(set = 0, binding = 8) uniform sampler2D texture0;
layout(set = 0, binding = 9) uniform sampler2D texture1;

#ifdef EACP_VERTEX
layout(location = 0) in vec2 attr0;
layout(location = 1) in vec2 attr1;
layout(location = 2) in vec2 attr2;
layout(location = 3) in vec2 attr3;
layout(location = 4) in vec2 attr4;
layout(location = 5) in vec2 attr5;
layout(location = 6) in vec4 attr6;
layout(location = 0) out vec2 vary0;
layout(location = 1) out vec2 vary1;
layout(location = 2) out vec4 vary2;

void main()
{
    vec2 t0 = ((attr1 + ((attr0).x * attr2)) + ((attr0).y * attr3));
    gl_Position = vec4(((((t0).x / (uniforms.u0).x) * 2.0) - 1.0), (1.0 - (((t0).y / (uniforms.u0).y) * 2.0)), 0.0, 1.0);
    vary0 = (attr4 + (attr0 * (attr5 - attr4)));
    vary1 = t0;
    vary2 = attr6;
}
#endif

#ifdef EACP_FRAGMENT
layout(location = 0) in vec2 vary0;
layout(location = 1) in vec2 vary1;
layout(location = 2) in vec4 vary2;
layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 t0 = (texture(texture0, vary0) * vary2);
    vec2 t1 = ((vary1 - (uniforms.u1).xy) * (uniforms.u1).zw);
    vec2 t2 = abs((t1 - 0.5));
    fragColor = vec4((t0).x, (t0).y, (t0).z, ((t0).w * ((step((t2).x, 0.5) * step((t2).y, 0.5)) * (texture(texture1, ((uniforms.u2).xy + (t1 * (uniforms.u2).zw)))).x)));
}
#endif
