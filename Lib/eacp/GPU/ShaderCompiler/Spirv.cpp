#include "Backends.h"

#include <cstring>

// glslang, in process, over the one GLSL text compiled once per stage, as
// ShaderLibrary-Linux.cpp does at run time - for every target a Vulkan device
// may ask for, since which one it takes is known only on that device. Built
// where eacp-spirv is; Spirv-Default.cpp everywhere else.

namespace eacp::GPU::ShaderCompiler::Detail
{
Vector<Spirv::Target> spirvTargets()
{
    return {Spirv::Target::vulkan11Spirv13,
            Spirv::Target::vulkan11Spirv14,
            Spirv::Target::vulkan13Spirv16};
}

Blob compileSpirv(Spirv::Stage stage,
                  Spirv::Target target,
                  const std::string& source)
{
    auto result = Spirv::compileGlsl(stage, source, target);
    auto blob = Blob {};
    blob.log = result.log;

    if (!result.succeeded())
        return blob;

    auto byteCount = result.words.getSize() * sizeof(std::uint32_t);
    blob.bytes.resize(byteCount);
    std::memcpy(blob.bytes.data(), result.words.data(), byteCount);
    blob.succeeded = true;
    return blob;
}
} // namespace eacp::GPU::ShaderCompiler::Detail
