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
layout(location = 0) in float attr0;
layout(location = 1) in vec2 attr1;
layout(location = 2) in vec2 attr2;
layout(location = 3) in vec2 attr3;
layout(location = 4) in vec4 attr4;
layout(location = 5) in vec4 attr5;
layout(location = 6) in vec4 attr6;
layout(location = 7) in vec4 attr7;
layout(location = 0) out vec4 vary0;
layout(location = 1) out float vary1;
layout(location = 2) out vec2 vary2;
layout(location = 3) out vec4 vary3;
layout(location = 4) out vec4 vary4;
layout(location = 5) out float vary5;

void main()
{
    float t0 = step(0.5, attr0);
    float t1 = step(1.5, attr0);
    vec2 t2 = mix(mix(attr1, attr2, vec2(t0)), attr3, vec2(t1));
    gl_Position = vec4(((((t2).x / (uniforms.u0).x) * 2.0) - 1.0), (1.0 - (((t2).y / (uniforms.u0).y) * 2.0)), 0.0, 1.0);
    vary0 = attr5;
    vary1 = mix(mix((attr4).x, (attr4).y, t0), (attr4).z, t1);
    vary2 = t2;
    vary3 = attr6;
    vary4 = attr7;
    vary5 = (attr4).w;
}
#endif

#ifdef EACP_FRAGMENT
layout(location = 0) in vec4 vary0;
layout(location = 1) in float vary1;
layout(location = 2) in vec2 vary2;
layout(location = 3) in vec4 vary3;
layout(location = 4) in vec4 vary4;
layout(location = 5) in float vary5;
layout(location = 0) out vec4 fragColor;

void main()
{
    float t0 = ((((vary3).x * (vary2).x) + ((vary3).y * (vary2).y)) + (vary4).x);
    float t1 = mix(t0, length(vec2(t0, ((((vary3).z * (vary2).x) + ((vary3).w * (vary2).y)) + (vary4).y))), step(1.5, vary5));
    vec4 t2 = mix(vary0, texture(texture1, vec2((0.00195312 + (mix(mix(clamp(t1, 0.0, 1.0), (1.0 - abs((1.0 - (t1 - (2.0 * floor((t1 / 2.0))))))), step(0.5, (vary4).w)), (t1 - floor(t1)), step(1.5, (vary4).w)) * 0.996094)), (vary4).z)), vec4(step(0.5, vary5)));
    vec2 t3 = ((vary2 - (uniforms.u1).xy) * (uniforms.u1).zw);
    vec2 t4 = abs((t3 - 0.5));
    fragColor = vec4((t2).x, (t2).y, (t2).z, ((t2).w * (vary1 * ((step((t4).x, 0.5) * step((t4).y, 0.5)) * (texture(texture0, ((uniforms.u2).xy + (t3 * (uniforms.u2).zw)))).x))));
}
#endif
