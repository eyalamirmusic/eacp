#include "ShaderLibrary.h"

#include "../Device/Device.h"
#include "../Linux/GPUBackend-Linux.h"
#include "ShaderSource.h"

namespace eacp::GPU
{
struct ShaderLibrary::Native
{
    Native(Device& device, const ShaderSource& source)
        : backend(getDeviceBackend(device).makeShaderLibrary(device, source))
    {
    }

    std::unique_ptr<ShaderLibraryBackend> backend;
};

ShaderLibrary::ShaderLibrary(Device& device, const ShaderSource& source)
    : vertexEntryName(source.vertexEntry)
    , fragmentEntryName(source.fragmentEntry)
    , computeEntryName(source.computeEntry)
    , groupShape(source.threadGroup)
    , impl(device, source)
{
}

bool ShaderLibrary::isValid() const
{
    return impl->backend->isValid();
}

void* ShaderLibrary::nativeLibrary() const
{
    return impl->backend->nativeLibrary();
}
} // namespace eacp::GPU
