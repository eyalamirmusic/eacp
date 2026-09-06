#version 450

// One string carries both stages: the compiler defines EACP_VERTEX or
// EACP_FRAGMENT for the stage it is building, so the two halves of the
// interface cannot drift apart. Vertex attribute i takes location i, mirroring
// the order of the attributes in the RenderPipelineDescriptor's VertexLayout,
// and a varying takes the same location on both sides.

#ifdef EACP_VERTEX
layout(location = 0) in vec2 attr0;
layout(location = 1) in vec3 attr1;
layout(location = 0) out vec3 vary0;

void main()
{
    gl_Position = vec4(attr0, 0.0, 1.0);
    vary0 = attr1;
}
#endif

#ifdef EACP_FRAGMENT
layout(location = 0) in vec3 vary0;
layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = vec4(vary0, 1.0);
}
#endif
