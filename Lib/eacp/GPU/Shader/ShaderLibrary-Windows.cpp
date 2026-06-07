#include "ShaderLibrary.h"

#include "../Device/Device.h"
#include "ShaderSource.h"

// Windows/DirectX stub. See Device-Windows.cpp.

namespace eacp::GPU
{
struct ShaderLibrary::Native
{
};

ShaderLibrary::ShaderLibrary(Device&, const ShaderSource& source)
    : vertexEntryName(source.vertexEntry)
    , fragmentEntryName(source.fragmentEntry)
    , impl()
{
}

bool ShaderLibrary::isValid() const
{
    return false;
}

void* ShaderLibrary::nativeLibrary() const
{
    return nullptr;
}
} // namespace eacp::GPU
