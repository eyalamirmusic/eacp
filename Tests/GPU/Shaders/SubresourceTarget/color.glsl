#version 450

#ifdef EACP_VERTEX
layout(location = 0) in vec3 attr0;
layout(location = 1) in vec4 attr1;
layout(location = 0) out vec4 color;

void main()
{
    gl_Position = vec4(attr0, 1.0);
    color = attr1;
}
#endif

#ifdef EACP_FRAGMENT
layout(location = 0) in vec4 color;
layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = color;
}
#endif
