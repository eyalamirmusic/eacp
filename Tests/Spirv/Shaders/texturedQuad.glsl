#version 450

layout(std140, set = 0, binding = 0) uniform Uniforms
{
    mat4 modelViewProjection;
    vec4 tint;
} uniforms;

layout(set = 0, binding = 8) uniform sampler2D albedo;

vec4 tinted(vec4 colour)
{
    return colour * uniforms.tint;
}

#ifdef EACP_VERTEX
layout(location = 0) in vec3 position;
layout(location = 1) in vec2 texCoord;
layout(location = 0) out vec2 varyingTexCoord;

void main()
{
    varyingTexCoord = texCoord;
    gl_Position = uniforms.modelViewProjection * vec4(position, 1.0);
}
#endif

#ifdef EACP_FRAGMENT
layout(location = 0) in vec2 varyingTexCoord;
layout(location = 0) out vec4 fragmentColour;

void main()
{
    fragmentColour = tinted(texture(albedo, varyingTexCoord));
}
#endif
