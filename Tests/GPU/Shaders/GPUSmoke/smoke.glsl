#version 450

#ifdef EACP_VERTEX
layout(location = 0) in vec4 attr0;

void main()
{
    gl_Position = attr0;
}
#endif

#ifdef EACP_FRAGMENT
layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = vec4(1.0, 1.0, 1.0, 1.0);
}
#endif
