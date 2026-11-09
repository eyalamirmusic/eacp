#pragma once

#import <Metal/Metal.h>

#include "../Shader/PipelineConstants.h"

#include <string>

namespace eacp::GPU
{
class ShaderLibrary;
}

namespace eacp::GPU::Detail
{
// The library's function of that name, +1, or nil. A library declaring
// pipeline-overridable constants has its function specialised with
// `constants` - MTLFunctionConstantValues at each constant's index, the ones
// left out falling back to the shader's defaults - which is the only way
// Metal hands out such a function a pipeline can be built from.
id<MTLFunction> makeMetalFunction(const ShaderLibrary& library,
                                  const std::string& name,
                                  const PipelineConstants& constants);
} // namespace eacp::GPU::Detail
