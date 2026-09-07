#include "RenderPipeline.h"

#include "../Device/Device.h"
#include "../Shader/ShaderLibrary.h"
#include "../Vulkan/VulkanTypes.h"

namespace eacp::GPU
{
namespace
{
VkPrimitiveTopology toVkTopology(PrimitiveTopology topology)
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

// UNORM/SNORM, not UINT/SINT: the shader reads these as 0..1 and -1..1.
VkFormat toVkVertexFormat(VertexFormat format)
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
        case VertexFormat::UByte4Norm:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case VertexFormat::Half2:
            return VK_FORMAT_R16G16_SFLOAT;
        case VertexFormat::Half4:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        case VertexFormat::Short2Norm:
            return VK_FORMAT_R16G16_SNORM;
        case VertexFormat::Short4Norm:
            return VK_FORMAT_R16G16B16A16_SNORM;
    }

    return VK_FORMAT_R32G32B32_SFLOAT;
}

StepRate stepRateForSlot(const VertexLayout& layout, int slot)
{
    if (slot >= 0 && slot < layout.buffers.size())
        return layout.buffers[slot].stepRate;

    return StepRate::PerVertex;
}

// Attribute i takes location i, which is what the GLSL emitter prints.
Vector<VkVertexInputAttributeDescription> makeAttributes(const VertexLayout& layout)
{
    auto attributes = Vector<VkVertexInputAttributeDescription> {};

    for (auto i = 0; i < layout.attributes.size(); ++i)
    {
        const auto& attribute = layout.attributes[i];

        VkVertexInputAttributeDescription entry = {};
        entry.location = static_cast<std::uint32_t>(i);
        entry.binding = static_cast<std::uint32_t>(attribute.bufferIndex);
        entry.format = toVkVertexFormat(attribute.format);
        entry.offset = static_cast<std::uint32_t>(attribute.offset);

        attributes.add(entry);
    }

    return attributes;
}

Vector<std::uint32_t> makeStrideTable(const VertexLayout& layout)
{
    auto strides = Vector<std::uint32_t> {};

    if (!layout.buffers.empty())
    {
        for (auto i = 0; i < layout.buffers.size(); ++i)
            strides.add(static_cast<std::uint32_t>(layout.buffers[i].stride));

        return strides;
    }

    strides.add(static_cast<std::uint32_t>(layout.stride));

    return strides;
}

// Only the slots an attribute names: every attribute must describe a listed
// binding, and every listed binding must have a buffer bound at the draw.
Vector<VkVertexInputBindingDescription>
    makeBindings(const VertexLayout& layout, const VulkanRenderPipeline& state)
{
    auto bindings = Vector<VkVertexInputBindingDescription> {};

    const auto alreadyListed = [&](int slot)
    {
        for (const auto& listed: bindings)
            if (listed.binding == static_cast<std::uint32_t>(slot))
                return true;

        return false;
    };

    for (const auto& attribute: layout.attributes)
    {
        const auto slot = attribute.bufferIndex;

        if (slot < 0 || alreadyListed(slot))
            continue;

        VkVertexInputBindingDescription entry = {};
        entry.binding = static_cast<std::uint32_t>(slot);
        entry.stride = state.strideForSlot(slot);
        entry.inputRate = stepRateForSlot(layout, slot) == StepRate::PerInstance
                              ? VK_VERTEX_INPUT_RATE_INSTANCE
                              : VK_VERTEX_INPUT_RATE_VERTEX;

        bindings.add(entry);
    }

    return bindings;
}

VkCullModeFlags toVkCullMode(CullMode mode)
{
    switch (mode)
    {
        case CullMode::None:
            return VK_CULL_MODE_NONE;
        case CullMode::Front:
            return VK_CULL_MODE_FRONT_BIT;
        case CullMode::Back:
            return VK_CULL_MODE_BACK_BIT;
    }

    return VK_CULL_MODE_NONE;
}

// No inversion: the negative viewport height RenderPass::setViewport sets has
// already flipped the winding.
VkFrontFace toVkFrontFace(Winding winding)
{
    return winding == Winding::CounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                : VK_FRONT_FACE_CLOCKWISE;
}

VkCompareOp toVkCompareOp(CompareFunction compare)
{
    switch (compare)
    {
        case CompareFunction::Never:
            return VK_COMPARE_OP_NEVER;
        case CompareFunction::Less:
            return VK_COMPARE_OP_LESS;
        case CompareFunction::LessEqual:
            return VK_COMPARE_OP_LESS_OR_EQUAL;
        case CompareFunction::Equal:
            return VK_COMPARE_OP_EQUAL;
        case CompareFunction::NotEqual:
            return VK_COMPARE_OP_NOT_EQUAL;
        case CompareFunction::GreaterEqual:
            return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case CompareFunction::Greater:
            return VK_COMPARE_OP_GREATER;
        case CompareFunction::Always:
            return VK_COMPARE_OP_ALWAYS;
    }

    return VK_COMPARE_OP_LESS_OR_EQUAL;
}

VkStencilOp toVkStencilOp(StencilOp op)
{
    switch (op)
    {
        case StencilOp::Keep:
            return VK_STENCIL_OP_KEEP;
        case StencilOp::Zero:
            return VK_STENCIL_OP_ZERO;
        case StencilOp::Replace:
            return VK_STENCIL_OP_REPLACE;
        case StencilOp::IncrementClamp:
            return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case StencilOp::DecrementClamp:
            return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case StencilOp::Invert:
            return VK_STENCIL_OP_INVERT;
        case StencilOp::IncrementWrap:
            return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case StencilOp::DecrementWrap:
            return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    }

    return VK_STENCIL_OP_KEEP;
}

VkStencilOpState toVkStencilFace(const StencilFace& face,
                                 const RenderPipelineDescriptor& from)
{
    VkStencilOpState state = {};

    state.failOp = toVkStencilOp(face.stencilFail);
    state.passOp = toVkStencilOp(face.pass);
    state.depthFailOp = toVkStencilOp(face.depthFail);
    state.compareOp = toVkCompareOp(face.compare);
    state.compareMask = from.stencilReadMask;
    state.writeMask = from.stencilWriteMask;

    return state;
}

VkBlendFactor toVkBlendFactor(BlendFactor factor)
{
    switch (factor)
    {
        case BlendFactor::Zero:
            return VK_BLEND_FACTOR_ZERO;
        case BlendFactor::One:
            return VK_BLEND_FACTOR_ONE;
        case BlendFactor::SourceColor:
            return VK_BLEND_FACTOR_SRC_COLOR;
        case BlendFactor::OneMinusSourceColor:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case BlendFactor::SourceAlpha:
            return VK_BLEND_FACTOR_SRC_ALPHA;
        case BlendFactor::OneMinusSourceAlpha:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::DestinationColor:
            return VK_BLEND_FACTOR_DST_COLOR;
        case BlendFactor::OneMinusDestinationColor:
            return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case BlendFactor::DestinationAlpha:
            return VK_BLEND_FACTOR_DST_ALPHA;
        case BlendFactor::OneMinusDestinationAlpha:
            return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case BlendFactor::SourceAlphaSaturated:
            return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    }

    return VK_BLEND_FACTOR_ONE;
}

VkBlendOp toVkBlendOperation(BlendOperation operation)
{
    switch (operation)
    {
        case BlendOperation::Add:
            return VK_BLEND_OP_ADD;
        case BlendOperation::Subtract:
            return VK_BLEND_OP_SUBTRACT;
        case BlendOperation::ReverseSubtract:
            return VK_BLEND_OP_REVERSE_SUBTRACT;
        case BlendOperation::Min:
            return VK_BLEND_OP_MIN;
        case BlendOperation::Max:
            return VK_BLEND_OP_MAX;
    }

    return VK_BLEND_OP_ADD;
}

VkColorComponentFlags toVkWriteMask(const ColorWriteMask& mask)
{
    auto value = VkColorComponentFlags {0};

    if (mask.red)
        value |= VK_COLOR_COMPONENT_R_BIT;
    if (mask.green)
        value |= VK_COLOR_COMPONENT_G_BIT;
    if (mask.blue)
        value |= VK_COLOR_COMPONENT_B_BIT;
    if (mask.alpha)
        value |= VK_COLOR_COMPONENT_A_BIT;

    return value;
}

VkPipelineColorBlendAttachmentState
    makeBlendAttachment(const RenderPipelineDescriptor& from)
{
    VkPipelineColorBlendAttachmentState attachment = {};
    attachment.colorWriteMask = toVkWriteMask(from.colorWriteMask);

    const auto blend = from.blend ? *from.blend : blendStateFor(from.blendMode);

    if (!blend.enabled)
        return attachment;

    attachment.blendEnable = VK_TRUE;
    attachment.srcColorBlendFactor = toVkBlendFactor(blend.sourceColor);
    attachment.dstColorBlendFactor = toVkBlendFactor(blend.destinationColor);
    attachment.colorBlendOp = toVkBlendOperation(blend.colorOperation);
    attachment.srcAlphaBlendFactor = toVkBlendFactor(blend.sourceAlpha);
    attachment.dstAlphaBlendFactor = toVkBlendFactor(blend.destinationAlpha);
    attachment.alphaBlendOp = toVkBlendOperation(blend.alphaOperation);

    return attachment;
}

VkPipelineRasterizationStateCreateInfo
    makeRasterizationState(const RenderPipelineDescriptor& from)
{
    VkPipelineRasterizationStateCreateInfo state = {};
    state.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    state.polygonMode = VK_POLYGON_MODE_FILL;
    state.cullMode = toVkCullMode(from.cullMode);
    state.frontFace = toVkFrontFace(from.frontFace);
    state.lineWidth = 1.0f;

    return state;
}

VkPipelineDepthStencilStateCreateInfo
    makeDepthStencilState(const RenderPipelineDescriptor& from)
{
    VkPipelineDepthStencilStateCreateInfo state = {};
    state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

    if (from.depth)
    {
        state.depthTestEnable = VK_TRUE;
        state.depthWriteEnable = from.depthWrite ? VK_TRUE : VK_FALSE;
        state.depthCompareOp = toVkCompareOp(from.depthCompare);
    }

    if (from.stencil)
    {
        state.stencilTestEnable = VK_TRUE;
        state.front = toVkStencilFace(from.stencilFront, from);
        state.back = toVkStencilFace(from.stencilBack, from);
    }

    return state;
}
} // namespace

struct RenderPipeline::Native
{
    Native(Device& device, const RenderPipelineDescriptor& descriptor)
        : topology(descriptor.topology)
        , cullMode(descriptor.cullMode)
        , frontFace(descriptor.frontFace)
    {
        state.topology = toVkTopology(descriptor.topology);
        state.cullMode = toVkCullMode(descriptor.cullMode);
        state.frontFace = toVkFrontFace(descriptor.frontFace);
        state.strides = makeStrideTable(descriptor.vertexLayout);
        state.depth = descriptor.depth;
        state.stencil = descriptor.stencil;
        state.sampleCount = descriptor.sampleCount > 1 ? descriptor.sampleCount : 1;
        state.colorFormat = toVkFormat(descriptor.colorFormat);

        if (!device.isValid() || descriptor.library == nullptr)
            return;

        context = &getVulkanContext(device);

        if (!device.supportsSampleCount(state.sampleCount))
        {
            LOG("Vulkan: no render pipeline at ",
                state.sampleCount,
                "x MSAA - the device does not support that sample count");
            return;
        }

        const auto& layouts = getVulkanShared().getRenderLayouts();

        if (!layouts.isValid())
            return;

        auto* program =
            static_cast<VulkanShaderProgram*>(descriptor.library->nativeLibrary());

        if (program == nullptr || program->vertex == VK_NULL_HANDLE
            || program->fragment == VK_NULL_HANDLE)
            return;

        build(*program, layouts, descriptor);
    }

    // Deferred: a command buffer that bound this pipeline may still be in flight.
    ~Native()
    {
        if (context == nullptr || state.pipeline == VK_NULL_HANDLE)
            return;

        context->deferRelease(
            [device = context->getDevice(), pipeline = state.pipeline]
            { vkDestroyPipeline(device, pipeline, nullptr); });
    }

    void build(const VulkanShaderProgram& program,
               const PipelineLayouts& layouts,
               const RenderPipelineDescriptor& descriptor)
    {
        VkPipelineShaderStageCreateInfo stages[2] = {};

        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = program.vertex;
        stages[0].pName = "main";

        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = program.fragment;
        stages[1].pName = "main";

        auto attributes = makeAttributes(descriptor.vertexLayout);
        auto bindings = makeBindings(descriptor.vertexLayout, state);

        VkPipelineVertexInputStateCreateInfo vertexInput = {};
        vertexInput.sType =
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount =
            static_cast<std::uint32_t>(bindings.size());
        vertexInput.pVertexBindingDescriptions = bindings.data();
        vertexInput.vertexAttributeDescriptionCount =
            static_cast<std::uint32_t>(attributes.size());
        vertexInput.pVertexAttributeDescriptions = attributes.data();

        VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
        inputAssembly.sType =
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = state.topology;

        VkPipelineViewportStateCreateInfo viewport = {};
        viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;

        auto rasterization = makeRasterizationState(descriptor);

        VkPipelineMultisampleStateCreateInfo multisample = {};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples =
            static_cast<VkSampleCountFlagBits>(state.sampleCount);

        auto depthStencil = makeDepthStencilState(descriptor);
        auto attachment = makeBlendAttachment(descriptor);

        VkPipelineColorBlendStateCreateInfo blend = {};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments = &attachment;

        const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                                VK_DYNAMIC_STATE_SCISSOR,
                                                VK_DYNAMIC_STATE_STENCIL_REFERENCE};

        VkPipelineDynamicStateCreateInfo dynamic = {};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(
            sizeof(dynamicStates) / sizeof(dynamicStates[0]));
        dynamic.pDynamicStates = dynamicStates;

        // The two planes are one attachment, so the depth format is declared
        // whenever either is asked for; it has to match what the pass binds.
        const auto hasDepthAttachment = descriptor.depth || descriptor.stencil;

        VkPipelineRenderingCreateInfo rendering = {};
        rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &state.colorFormat;
        rendering.depthAttachmentFormat =
            hasDepthAttachment ? depthAttachmentFormat(descriptor.stencil)
                               : VK_FORMAT_UNDEFINED;
        rendering.stencilAttachmentFormat =
            descriptor.stencil ? depthAttachmentFormat(true) : VK_FORMAT_UNDEFINED;

        VkGraphicsPipelineCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.pNext = &rendering;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &inputAssembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &rasterization;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depthStencil;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = layouts.pipelineLayout;

        if (vkCreateGraphicsPipelines(context->getDevice(),
                                      getVulkanShared().getPipelineCache(),
                                      1,
                                      &info,
                                      nullptr,
                                      &state.pipeline)
            != VK_SUCCESS)
        {
            state.pipeline = VK_NULL_HANDLE;
            return;
        }

        state.layout = layouts.pipelineLayout;
        state.setLayout = layouts.setLayout;
    }

    VulkanContext* context = nullptr;
    PrimitiveTopology topology = PrimitiveTopology::Triangles;
    CullMode cullMode = CullMode::None;
    Winding frontFace = Winding::CounterClockwise;
    VulkanRenderPipeline state;
};

RenderPipeline::RenderPipeline(Device& device,
                               const RenderPipelineDescriptor& descriptor)
    : impl(device, descriptor)
{
}

bool RenderPipeline::isValid() const
{
    return impl->state.pipeline != VK_NULL_HANDLE;
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
    return const_cast<VulkanRenderPipeline*>(&impl->state);
}

void* RenderPipeline::nativeDepthState() const
{
    return impl->state.depth ? const_cast<VulkanRenderPipeline*>(&impl->state)
                             : nullptr;
}
} // namespace eacp::GPU
