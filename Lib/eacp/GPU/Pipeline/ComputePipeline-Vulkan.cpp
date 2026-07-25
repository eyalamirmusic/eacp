#include "ComputePipeline.h"

#include "../Device/Device.h"
#include "../Shader/ShaderLibrary.h"
#include "../Vulkan/VulkanTypes.h"

// Compute is not part of the Vulkan prototype yet: a kernel binds its storage
// buffers through a descriptor set, and this backend has no descriptor plumbing
// so far. Pipelines report invalid rather than pretending, so the compute smoke
// tests skip the way they do without a device at all.

namespace eacp::GPU
{
struct ComputePipeline::Native
{
    VulkanPipeline pipeline;
};

ComputePipeline::ComputePipeline(Device&, const ShaderLibrary&)
    : impl()
{
}

bool ComputePipeline::isValid() const
{
    return false;
}

void* ComputePipeline::nativeState() const
{
    return nullptr;
}
} // namespace eacp::GPU
