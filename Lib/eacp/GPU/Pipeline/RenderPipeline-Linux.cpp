#include "RenderPipeline.h"

#include "../Device/Device.h"
#include "../Vulkan/VulkanTypes.h"

// Linux/Vulkan placeholder. Stage 3 of the Linux plan replaces this file with
// the real thing: a VkGraphicsPipeline built with dynamic rendering (no
// VkRenderPass), the vertex layout translated from VertexLayout, blend, depth,
// stencil and cull state translated from the descriptor, and the render
// descriptor-set layout that Codegen/ShaderBindings.h already describes.
//
// Until then a pipeline has nothing behind it and says so. The fixed-function
// state a pass would read is still answered from the descriptor, because those
// three are the caller's own values coming straight back rather than anything
// about a pipeline that does not exist - which is how the Metal backend answers
// them too.

namespace eacp::GPU
{
struct RenderPipeline::Native
{
    Native(Device&, const RenderPipelineDescriptor& descriptor)
        : topology(descriptor.topology)
        , cullMode(descriptor.cullMode)
        , frontFace(descriptor.frontFace)
    {
    }

    PrimitiveTopology topology = PrimitiveTopology::Triangles;
    CullMode cullMode = CullMode::None;
    Winding frontFace = Winding::CounterClockwise;
};

RenderPipeline::RenderPipeline(Device& device,
                               const RenderPipelineDescriptor& descriptor)
    : impl(device, descriptor)
{
}

bool RenderPipeline::isValid() const
{
    return false;
}

PrimitiveTopology RenderPipeline::topology() const
{
    return impl->topology;
}

CullMode RenderPipeline::cullMode() const
{
    return impl->cullMode;
}

Winding RenderPipeline::frontFace() const
{
    return impl->frontFace;
}

void* RenderPipeline::nativeState() const
{
    return nullptr;
}

void* RenderPipeline::nativeDepthState() const
{
    return nullptr;
}
} // namespace eacp::GPU
