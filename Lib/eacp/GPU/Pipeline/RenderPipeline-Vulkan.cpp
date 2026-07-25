#include "RenderPipeline.h"

#include "../Device/Device.h"
#include "../Shader/ShaderLibrary.h"
#include "../Vulkan/VulkanContext.h"
#include "../Vulkan/VulkanTypes.h"

namespace eacp::GPU
{
namespace
{
VkFormat toVulkan(VertexFormat format)
{
    switch (format)
    {
        case VertexFormat::Float:
            return VK_FORMAT_R32_SFLOAT;
        case VertexFormat::Float2:
            return VK_FORMAT_R32G32_SFLOAT;
        case VertexFormat::Float3:
            return VK_FORMAT_R32G32B32_SFLOAT;
        case VertexFormat::Float4:
            return VK_FORMAT_R32G32B32A32_SFLOAT;
    }

    return VK_FORMAT_R32G32B32_SFLOAT;
}

VkPrimitiveTopology toVulkan(PrimitiveTopology topology)
{
    switch (topology)
    {
        case PrimitiveTopology::Triangles:
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case PrimitiveTopology::TriangleStrip:
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case PrimitiveTopology::Lines:
            return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case PrimitiveTopology::LineStrip:
            return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case PrimitiveTopology::Points:
            return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    }

    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

void applyBlend(VkPipelineColorBlendAttachmentState& state, BlendMode mode)
{
    state.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                           | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    if (mode == BlendMode::None)
    {
        state.blendEnable = VK_FALSE;
        return;
    }

    state.blendEnable = VK_TRUE;
    state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    state.dstColorBlendFactor = mode == BlendMode::AlphaBlend
                                    ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
                                    : VK_BLEND_FACTOR_ONE;
    state.colorBlendOp = VK_BLEND_OP_ADD;
    state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    state.dstAlphaBlendFactor = mode == BlendMode::AlphaBlend
                                    ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
                                    : VK_BLEND_FACTOR_ONE;
    state.alphaBlendOp = VK_BLEND_OP_ADD;
}
} // namespace

struct RenderPipeline::Native
{
    explicit Native(const RenderPipelineDescriptor& descriptor)
        : topologyValue(descriptor.topology)
    {
        auto& context = getVulkanContext();

        if (!context.isValid() || descriptor.library == nullptr
            || !descriptor.library->isValid())
            return;

        auto* device = context.getDevice();
        auto* program =
            static_cast<VulkanShaderProgram*>(descriptor.library->nativeLibrary());

        // Every render pipeline shares one layout: uniforms as push constants
        // bound to both stages (matching how RenderPass::draw binds the same
        // block to vertex and fragment), plus the texture descriptor set. Shared
        // rather than per-pipeline so a bound descriptor set survives a pipeline
        // change instead of being invalidated by it.
        pipeline.layout = context.getRenderPipelineLayout();
        pipeline.pushConstantBytes = context.maxUniformBytes();

        auto stages = Array<VkPipelineShaderStageCreateInfo, 2> {};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = program->module;
        stages[0].pName = descriptor.library->vertexEntry().c_str();
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = program->module;
        stages[1].pName = descriptor.library->fragmentEntry().c_str();

        auto bindings = Vector<VkVertexInputBindingDescription> {};
        const auto& layout = descriptor.vertexLayout;

        if (layout.buffers.size() > 0)
        {
            for (auto i = 0; i < layout.buffers.size(); ++i)
            {
                auto binding = VkVertexInputBindingDescription {};
                binding.binding = static_cast<std::uint32_t>(i);
                binding.stride =
                    static_cast<std::uint32_t>(layout.buffers[i].stride);
                binding.inputRate =
                    layout.buffers[i].stepRate == StepRate::PerInstance
                        ? VK_VERTEX_INPUT_RATE_INSTANCE
                        : VK_VERTEX_INPUT_RATE_VERTEX;
                bindings.add(binding);
            }
        }
        else if (layout.stride > 0)
        {
            auto binding = VkVertexInputBindingDescription {};
            binding.stride = static_cast<std::uint32_t>(layout.stride);
            binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
            bindings.add(binding);
        }

        auto attributes = Vector<VkVertexInputAttributeDescription> {};

        for (auto i = 0; i < layout.attributes.size(); ++i)
        {
            auto attribute = VkVertexInputAttributeDescription {};
            attribute.location = static_cast<std::uint32_t>(i);
            attribute.binding =
                static_cast<std::uint32_t>(layout.attributes[i].bufferIndex);
            attribute.format = toVulkan(layout.attributes[i].format);
            attribute.offset =
                static_cast<std::uint32_t>(layout.attributes[i].offset);
            attributes.add(attribute);
        }

        auto vertexInput = VkPipelineVertexInputStateCreateInfo {
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertexInput.vertexBindingDescriptionCount =
            static_cast<std::uint32_t>(bindings.size());
        vertexInput.pVertexBindingDescriptions =
            bindings.size() > 0 ? &bindings[0] : nullptr;
        vertexInput.vertexAttributeDescriptionCount =
            static_cast<std::uint32_t>(attributes.size());
        vertexInput.pVertexAttributeDescriptions =
            attributes.size() > 0 ? &attributes[0] : nullptr;

        auto assembly = VkPipelineInputAssemblyStateCreateInfo {
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = toVulkan(descriptor.topology);

        auto viewport = VkPipelineViewportStateCreateInfo {
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;

        auto raster = VkPipelineRasterizationStateCreateInfo {
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.f;

        auto multisample = VkPipelineMultisampleStateCreateInfo {
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples =
            static_cast<VkSampleCountFlagBits>(descriptor.sampleCount);

        auto depthStencil = VkPipelineDepthStencilStateCreateInfo {
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depthStencil.depthTestEnable = descriptor.depth ? VK_TRUE : VK_FALSE;
        depthStencil.depthWriteEnable = descriptor.depth ? VK_TRUE : VK_FALSE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

        auto blendAttachment = VkPipelineColorBlendAttachmentState {};
        applyBlend(blendAttachment, descriptor.blendMode);

        auto blend = VkPipelineColorBlendStateCreateInfo {
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;

        auto dynamicStates = Array<VkDynamicState, 2> {VK_DYNAMIC_STATE_VIEWPORT,
                                                       VK_DYNAMIC_STATE_SCISSOR};

        auto dynamic = VkPipelineDynamicStateCreateInfo {
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamicStates.data();

        auto colorFormat = VkFormat {VK_FORMAT_B8G8R8A8_UNORM};

        auto rendering = VkPipelineRenderingCreateInfoKHR {
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &colorFormat;

        if (descriptor.depth)
            rendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;

        auto info = VkGraphicsPipelineCreateInfo {
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.pNext = &rendering;
        info.stageCount = 2;
        info.pStages = stages.data();
        info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depthStencil;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = pipeline.layout;

        vkCreateGraphicsPipelines(
            device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline.pipeline);
    }

    ~Native()
    {
        auto& context = getVulkanContext();

        if (!context.isValid())
            return;

        if (pipeline.pipeline != VK_NULL_HANDLE)
            context.deferDestroy(pipeline.pipeline);

        // The layout belongs to the context and outlives every pipeline.
    }

    VulkanPipeline pipeline;
    PrimitiveTopology topologyValue = PrimitiveTopology::Triangles;
};

RenderPipeline::RenderPipeline(Device&, const RenderPipelineDescriptor& descriptor)
    : impl(descriptor)
{
}

bool RenderPipeline::isValid() const
{
    return impl->pipeline.pipeline != VK_NULL_HANDLE;
}

PrimitiveTopology RenderPipeline::topology() const
{
    return impl->topologyValue;
}

void* RenderPipeline::nativeState() const
{
    if (!isValid())
        return nullptr;

    return const_cast<VulkanPipeline*>(&impl->pipeline);
}

void* RenderPipeline::nativeDepthState() const
{
    // Depth state is baked into the pipeline object on Vulkan, so there is no
    // separate handle to hand back the way Metal has MTLDepthStencilState.
    return nullptr;
}
} // namespace eacp::GPU
