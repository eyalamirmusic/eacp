#version 450
#extension GL_ARB_shader_viewport_layer_array : require

#ifdef EACP_VERTEX
layout(location = 0) in vec3 attr0;
layout(location = 1) in vec4 attr1;
layout(location = 0) out vec4 color;

void main()
{
    gl_Position = vec4(attr0, 1.0);
    color = vec4(float(gl_InstanceIndex + 1) * 0.2, 0.0, 0.0, 1.0);
    gl_Layer = gl_InstanceIndex;
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
