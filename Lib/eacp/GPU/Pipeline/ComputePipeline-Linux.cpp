#include "ComputePipeline.h"

#include "../Device/Device.h"
#include "../Linux/GPUBackend-Linux.h"
#include "../Shader/ShaderLibrary.h"

namespace eacp::GPU
{
struct ComputePipeline::Native
{
    Native(Device& device, const ShaderLibrary& library)
        : backend(getDeviceBackend(device).makeComputePipeline(device, library))
    {
    }

    std::unique_ptr<ComputePipelineBackend> backend;
};

ComputePipeline::ComputePipeline(Device& device, const ShaderLibrary& library)
    : groupShape(library.threadGroupShape())
    , impl(device, library)
{
}

bool ComputePipeline::isValid() const
{
    return impl->backend->isValid();
}

int ComputePipeline::threadExecutionWidth() const
{
    return impl->backend->threadExecutionWidth();
}

void* ComputePipeline::nativeState() const
{
    return impl->backend->nativeState();
}
} // namespace eacp::GPU
