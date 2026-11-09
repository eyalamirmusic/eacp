#include "Kernels.h"

#include <eacp/GPU/Codegen/ShaderSets.h>

// Every kernel the app dispatches, for eacp_add_precompiled_shaders to compile
// at build time. Built into the Compute-shaders tool, not the app.
EACP_SHADER_SET()
{
    using namespace ComputeExample;

    visit<ToneKernel, CrossfadeKernel, SmoothKernel>();
}
