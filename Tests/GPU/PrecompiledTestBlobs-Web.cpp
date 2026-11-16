#include "PrecompiledTestBlobs.h"

// WebGPU compiles WGSL in the page and takes no precompiled binary, so there is
// nothing to build ahead of time and the cases that need one stand down.
namespace eacp::GPU::PrecompiledTestBlobs
{
Vector<std::string> resourceNamesFor(const ShaderSource&)
{
    return {};
}

Resources compileAheadOfTime(const ShaderSource&)
{
    return {};
}
} // namespace eacp::GPU::PrecompiledTestBlobs
