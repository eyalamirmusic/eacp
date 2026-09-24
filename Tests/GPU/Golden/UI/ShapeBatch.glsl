#version 450

layout(std140, set = 0, binding = 0) uniform Uniforms
{
    vec2 u0;
    float u1;
    vec4 u2;
    vec4 u3;
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
layout(location = 7) in vec4 attr7;
layout(location = 8) in vec4 attr8;
layout(location = 9) in vec4 attr9;
layout(location = 10) in vec4 attr10;
layout(location = 11) in vec2 attr11;
layout(location = 12) in vec4 attr12;
layout(location = 0) out vec2 vary0;
layout(location = 1) out vec2 vary1;
layout(location = 2) out vec4 vary2;
layout(location = 3) out vec4 vary3;
layout(location = 4) out vec2 vary4;
layout(location = 5) out vec4 vary5;
layout(location = 6) out vec4 vary6;
layout(location = 7) out vec2 vary7;
layout(location = 8) out vec4 vary8;
layout(location = 9) out vec2 vary9;

void main()
{
    vec2 t0 = ((attr1 + ((attr0).x * attr2)) + ((attr0).y * attr3));
    gl_Position = vec4(((((t0).x / (uniforms.u0).x) * 2.0) - 1.0), (1.0 - (((t0).y / (uniforms.u0).y) * 2.0)), 0.0, 1.0);
    vary0 = ((attr0 - 0.5) * (attr5 * 2.0));
    vary1 = attr4;
    vary2 = attr6;
    vary3 = attr7;
    vary4 = t0;
    vary5 = attr9;
    vary6 = attr10;
    vary7 = attr11;
    vary8 = attr12;
    vary9 = ((attr8).xy + (attr0 * (attr8).zw));
}
#endif

#ifdef EACP_FRAGMENT
layout(location = 0) in vec2 vary0;
layout(location = 1) in vec2 vary1;
layout(location = 2) in vec4 vary2;
layout(location = 3) in vec4 vary3;
layout(location = 4) in vec2 vary4;
layout(location = 5) in vec4 vary5;
layout(location = 6) in vec4 vary6;
layout(location = 7) in vec2 vary7;
layout(location = 8) in vec4 vary8;
layout(location = 9) in vec2 vary9;
layout(location = 0) out vec4 fragColor;

void main()
{
    float t0 = ((((vary5).x * (vary4).x) + ((vary5).y * (vary4).y)) + (vary6).x);
    float t1 = mix(t0, length(vec2(t0, ((((vary5).z * (vary4).x) + ((vary5).w * (vary4).y)) + (vary6).y))), step(1.5, (vary3).w));
    vec4 t2 = mix(vary2, texture(texture1, vec2((0.00195312 + (mix(mix(clamp(t1, 0.0, 1.0), (1.0 - abs((1.0 - (t1 - (2.0 * floor((t1 / 2.0))))))), step(0.5, (vary6).w)), (t1 - floor(t1)), step(1.5, (vary6).w)) * 0.996094)), (vary6).z)), vec4(step(0.5, (vary3).w)));
    vec2 t3 = (abs(vary0) - (vary1 - vec2((vary3).x, (vary3).x)));
    float t4 = ((length(max(vec2(0.0), t3)) + min(0.0, max((t3).x, (t3).y))) - (vary3).x);
    float t5 = ((vary3).y * 0.5);
    float t6 = mix(t4, (abs((t4 + t5)) - t5), (vary3).z);
    float t7 = clamp((0.5 - (t6 * uniforms.u1)), 0.0, 1.0);
    float t8 = mix(t7, min(smoothstep(0.0, 1.0, clamp((0.5 - (t6 * (vary7).x)), 0.0, 1.0)), (smoothstep(0.0, 1.0, clamp((0.5 - ((abs((vary0).x) - (vary1).x) * (vary7).x)), 0.0, 1.0)) * smoothstep(0.0, 1.0, clamp((0.5 - ((abs((vary0).y) - (vary1).y) * (vary7).x)), 0.0, 1.0)))), step(0.0001, (vary7).x));
    float t9 = (max(0.0, ((vary3).x + (vary8).z)) * step(0.001, (vary3).x));
    vec2 t10 = (abs((vary0 - (vary8).xy)) - ((vary1 + vec2((vary8).z, (vary8).z)) - vec2(t9, t9)));
    float t11 = clamp((0.5 - (((length(max(vec2(0.0), t10)) + min(0.0, max((t10).x, (t10).y))) - t9) * uniforms.u1)), 0.0, 1.0);
    vec2 t12 = ((vary4 - (uniforms.u2).xy) * (uniforms.u2).zw);
    vec2 t13 = abs((t12 - 0.5));
    fragColor = vec4((t2).x, (t2).y, (t2).z, ((t2).w * ((mix(t7, mix((t8 * (1.0 - t11)), ((1.0 - t8) * t11), (vary8).w), (vary7).y) * (texture(texture0, vary9)).x) * ((step((t13).x, 0.5) * step((t13).y, 0.5)) * (texture(texture0, ((uniforms.u3).xy + (t12 * (uniforms.u3).zw)))).x))));
}
#endif
