#version 450

layout(std140, set = 0, binding = 0) uniform Uniforms
{
    vec2 u0;
    vec4 u1;
    vec4 u2;
    float u3;
    vec4 u4;
    vec2 u5;
    vec4 u6;
    vec4 u7;
} uniforms;

layout(set = 0, binding = 8) uniform sampler2D texture0;
layout(set = 0, binding = 9) uniform sampler2D texture1;

#ifdef EACP_VERTEX
layout(location = 0) in vec2 attr0;
layout(location = 0) out vec2 vary0;
layout(location = 1) out vec2 vary1;

void main()
{
    vec2 t0 = vec2(((attr0).x * (uniforms.u1).z), ((attr0).y * (uniforms.u1).w));
    vec2 t1 = vec2(((((uniforms.u4).x * (t0).x) + ((uniforms.u4).z * (t0).y)) + (uniforms.u5).x), ((((uniforms.u4).y * (t0).x) + ((uniforms.u4).w * (t0).y)) + (uniforms.u5).y));
    vec2 t2 = vec2(((uniforms.u1).x + (t1).x), ((uniforms.u1).y + (t1).y));
    gl_Position = vec4(((((t2).x / (uniforms.u0).x) * 2.0) - 1.0), (1.0 - (((t2).y / (uniforms.u0).y) * 2.0)), 0.0, 1.0);
    vary0 = vec2(((uniforms.u2).x + ((attr0).x * (uniforms.u2).z)), ((uniforms.u2).y + ((attr0).y * (uniforms.u2).w)));
    vary1 = t2;
}
#endif

#ifdef EACP_FRAGMENT
layout(location = 0) in vec2 vary0;
layout(location = 1) in vec2 vary1;
layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 t0 = texture(texture0, vary0);
    vec3 t1 = ((t0).xyz / max((t0).w, 0.00195312));
    vec2 t2 = ((vary1 - (uniforms.u6).xy) * (uniforms.u6).zw);
    vec2 t3 = abs((t2 - 0.5));
    fragColor = vec4((t1).x, (t1).y, (t1).z, (((t0).w * uniforms.u3) * ((step((t3).x, 0.5) * step((t3).y, 0.5)) * (texture(texture1, ((uniforms.u7).xy + (t2 * (uniforms.u7).zw)))).x)));
}
#endif
