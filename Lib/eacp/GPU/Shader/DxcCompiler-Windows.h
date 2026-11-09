#pragma once

#include "CompilationInfo.h"
#include "Dxc.h"
#include "PipelineConstants.h"

#include <eacp/Core/Utils/WinInclude.h>

#include <d3d12.h>

#include <winrt/base.h>

#include <memory>
#include <string>
#include <string_view>

// The half of Dxc.h a ShaderLibrary needs: which compiler a library built on a
// device compiles through, and the compile itself. Not part of GPU.h.
namespace eacp::GPU::Dxc::Detail
{
class Compiler;

// Null means FXC: DXC is disabled, could not be loaded, or the device has no
// Shader Model 6.0. The first call after the options were set loads it.
std::shared_ptr<const Compiler> compilerFor(ID3D12Device* device);

// One stage to signed DXIL in a blob, or null after logging DXC's diagnostics.
// Each define is a -D NAME=VALUE: a pipeline's overrides (PipelineConstants.h).
// What DXC said - its errors, or a successful compile's warnings - is added to
// `messages` as `stage`'s.
winrt::com_ptr<ID3DBlob> compile(const Compiler& compiler,
                                 const std::string& source,
                                 const std::string& entry,
                                 std::string_view profile,
                                 ShaderStage stage,
                                 const Vector<ShaderDefine>& defines,
                                 CompilationInfo& messages);
} // namespace eacp::GPU::Dxc::Detail
