#version 450

layout(std140, set = 0, binding = 0) uniform Uniforms
{
    vec2 u0;
    vec2 u1;
    vec2 u2;
    vec2 u3;
    vec4 u4;
    vec4 u5;
    vec4 u6;
} uniforms;

layout(set = 0, binding = 8) uniform sampler2D texture0;
layout(set = 0, binding = 9) uniform sampler2D texture1;

#ifdef EACP_VERTEX
layout(location = 0) in vec2 attr0;
layout(location = 0) out vec2 vary0;

void main()
{
    vec2 t0 = ((uniforms.u1 + ((attr0).x * uniforms.u2)) + ((attr0).y * uniforms.u3));
    gl_Position = vec4(((((t0).x / (uniforms.u0).x) * 2.0) - 1.0), (1.0 - (((t0).y / (uniforms.u0).y) * 2.0)), 0.0, 1.0);
    vary0 = attr0;
}
#endif

#ifdef EACP_FRAGMENT
layout(location = 0) in vec2 vary0;
layout(location = 0) out vec4 fragColor;

void main()
{
    float t0 = (((texture(texture0, vary0)).x - (uniforms.u5).x) * (uniforms.u5).y);
    vec4 t1 = texture(texture1, vary0);
    float t2 = (((t1).y - (uniforms.u5).z) * (uniforms.u5).w);
    float t3 = (((t1).x - (uniforms.u5).z) * (uniforms.u5).w);
    vec3 t4 = clamp(vec3((t0 + ((uniforms.u6).x * t2)), ((t0 - ((uniforms.u6).y * t3)) - ((uniforms.u6).z * t2)), (t0 + ((uniforms.u6).w * t3))), vec3(0.0), vec3(1.0));
    fragColor = (vec4((t4).x, (t4).y, (t4).z, 1.0) * uniforms.u4);
}
#endif
