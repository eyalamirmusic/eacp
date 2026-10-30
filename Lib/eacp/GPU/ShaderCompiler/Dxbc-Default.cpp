#include "Backends.h"

// FXC exists only on Windows.

namespace eacp::GPU::ShaderCompiler::Detail
{
bool isDxbcAvailable()
{
    return false;
}

Blob compileDxbc(const std::string&, const std::string&, const char*)
{
    return Blob {false, {}, "DXBC can only be compiled on a Windows host"};
}
} // namespace eacp::GPU::ShaderCompiler::Detail
