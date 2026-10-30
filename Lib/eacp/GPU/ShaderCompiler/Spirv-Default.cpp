#include "Backends.h"

// A build without eacp-spirv (EACP_BUILD_SPIRV off) has no SPIR-V compiler.

namespace eacp::GPU::ShaderCompiler::Detail
{
Vector<Spirv::Target> spirvTargets()
{
    return {};
}

Blob compileSpirv(Spirv::Stage, Spirv::Target, const std::string&)
{
    return Blob {
        false, {}, "this build carries no SPIR-V compiler (EACP_BUILD_SPIRV)"};
}
} // namespace eacp::GPU::ShaderCompiler::Detail
