#include "RotatingShader.h"

#include <eacp/GPU/Codegen/ShaderSets.h>

// Every shader the app draws with, for eacp_add_precompiled_shaders to compile
// at build time. Built into the RotatingTriangle-shaders tool, not the app.
EACP_SHADER_SET()
{
    visit<RotatingTriangle::RotatingShader>();
}
