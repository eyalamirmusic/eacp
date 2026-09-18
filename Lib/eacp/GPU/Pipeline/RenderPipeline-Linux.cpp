#include "RenderPipeline.h"

#include "../Device/Device.h"
#include "../Linux/GPUBackend-Linux.h"
#include "../Shader/ShaderLibrary.h"

namespace eacp::GPU
{
struct RenderPipeline::Native
{
    Native(Device& device, const RenderPipelineDescriptor& descriptor)
        : backend(getDeviceBackend(device).makeRenderPipeline(device, descriptor))
    {
    }

    std::unique_ptr<RenderPipelineBackend> backend;
};

RenderPipeline::RenderPipeline(Device& device,
                               const RenderPipelineDescriptor& descriptor)
    : impl(device, descriptor)
{
}

bool RenderPipeline::isValid() const
{
    return impl->backend->isValid();
}

PrimitiveTopology RenderPipeline::topology() const
{
    return impl->backend->topology();
}

CullMode RenderPipeline::cullMode() const
{
    return impl->backend->cullMode();
}

Winding RenderPipeline::frontFace() const
{
    return impl->backend->frontFace();
}

void* RenderPipeline::nativeState() const
{
    return impl->backend->nativeState();
}

void* RenderPipeline::nativeDepthState() const
{
    return impl->backend->nativeDepthState();
}
} // namespace eacp::GPU
