#version 450
#ifdef EACP_VERTEX
void main()
{
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
#endif
#ifdef EACP_FRAGMENT
layout(location = 0) out vec4 colour;

void main()
{
    colour = missingColour;
}
#endif
