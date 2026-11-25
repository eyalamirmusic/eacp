#version 450

#ifdef EACP_VERTEX
layout(location = 0) in vec3 attr0;
layout(set = 0, binding = 8) uniform sampler2DShadow depthImage;

void main()
{
    float passed = textureLod(depthImage, vec3(attr0.y, 0.5, 0.5), 0.0);
    float h = 0.5 + 0.5 * passed;
    gl_Position = vec4(attr0.x, -1.0 + 2.0 * h * attr0.z, 0.0, 1.0);
}
#endif

#ifdef EACP_FRAGMENT
layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = vec4(1.0);
}
#endif
