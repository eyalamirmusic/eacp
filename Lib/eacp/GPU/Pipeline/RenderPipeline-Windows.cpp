#include "RenderPipeline.h"

#include "../Device/Device.h"
#include "../Shader/ShaderLibrary.h"

// Windows/DirectX stub. See Device-Windows.cpp.

namespace eacp::GPU
{
struct RenderPipeline::Native
{
};

RenderPipeline::RenderPipeline(Device&, const RenderPipelineDescriptor&)
    : impl()
{
}

bool RenderPipeline::isValid() const
{
    return false;
}

void* RenderPipeline::nativeState() const
{
    return nullptr;
}
} // namespace eacp::GPU
