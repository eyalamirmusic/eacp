#version 450

#ifdef EACP_VERTEX
layout(location = 0) in vec2 attr0;

void main()
{
    gl_Position = vec4(attr0, 0.5, 1.0);
}
#endif

#ifdef EACP_FRAGMENT
layout(set = 0, binding = 8) uniform sampler2D depth;
layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = vec4(texelFetch(depth, ivec2(gl_FragCoord.xy), 0).r, 0.0, 0.0, 1.0);
}
#endif
