#version 450

#ifdef EACP_FRAGMENT
layout(location = 0) out vec4 fragmentColour;

void main()
{
    fragmentColour = vec4(1.0);
}
#endif
