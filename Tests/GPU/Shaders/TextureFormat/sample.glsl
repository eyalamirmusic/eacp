#version 450

#ifdef EACP_VERTEX
layout(location = 0) in vec2 attr0;
layout(location = 1) in vec2 attr1;
layout(location = 0) out vec2 uv;

void main()
{
    gl_Position = vec4(attr0, 0.0, 1.0);
    uv = attr1;
}
#endif

#ifdef EACP_FRAGMENT
layout(location = 0) in vec2 uv;
layout(set = 0, binding = 8) uniform sampler2D image;
layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = texture(image, uv);
}
#endif
