#include "RenderPipeline.h"

#include "../Device/Device.h"
#include "../Shader/ShaderLibrary.h"
#include "../Vulkan/VulkanTypes.h"

// Linux/Vulkan backend. Everything the descriptor names bakes into one
// VkPipeline against the shared render pipeline layout; only the topology, the
// per-slot strides and the three attachment facts stay outside it, read by the
// render pass at draw time.
//
// Dynamic rendering throughout: there is no VkRenderPass and no VkFramebuffer
// anywhere in this backend. A graphics pipeline still has to be told the
// attachment formats it will be used with, and VkPipelineRenderingCreateInfo in
// the pNext chain is where they go - the exact information
// D3D12_GRAPHICS_PIPELINE_STATE_DESC carries in RTVFormats and DSVFormat.
// VulkanShared asks for the dynamicRendering feature by name at device
// creation, so a device that reaches here has it.
//
// Three states are dynamic and nothing else is: viewport and scissor, which the
// pass sets per pass and per draw (and which carry the negative-height y flip
// this backend's coordinate convention rests on), and the stencil reference,
// which is RenderPass::setStencilReference. Cull mode and front face are baked,
// which is where this backend follows D3D12 rather than Metal - see
// makeRasterizationState.

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

// UNORM and SNORM rather than UINT and SINT, for the reason the D3D12 file
// gives: the shader reads these as 0..1 and -1..1, and the integer variants
// would deliver raw 0..255 and disagree with the other backends rather than
// fail to build.
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

// Multi-buffer layouts carry per-slot metadata; a legacy single-buffer layout
// (buffers empty) is always PerVertex at slot 0.
StepRate stepRateForSlot(const VertexLayout& layout, int slot)
{
    if (slot >= 0 && slot < layout.buffers.size())
        return layout.buffers[slot].stepRate;

    return StepRate::PerVertex;
}

// Attribute i takes location i, which is the location the GLSL emitter prints
// for `attr<i>` - the mirror of Metal's [[attribute(n)]] and of the D3D12
// backend's TEXCOORD<n>.
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

// The per-slot stride table the pipeline is built with and the pass reads back
// through VulkanRenderPipeline::strideForSlot. Taken from layout.buffers when
// present; a single-slot table from layout.stride otherwise, so single-buffer
// callers see no behavioural change.
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

// One binding per slot an attribute actually names, which is the set Vulkan
// asks for exactly: every attribute must describe a listed binding, and a
// listed binding a vertex shader reads must have a buffer bound at the draw. A
// slot with a stride and no attribute is neither, so it is left out - unlike
// D3D12, where the stride table alone decides and an unused slot costs nothing.
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

// Winding maps straight across, with no inversion, and that is a consequence of
// the viewport rather than a coincidence.
//
// Vulkan is the one API of the three whose NDC has y pointing down, and this
// backend answers that with a negative viewport height in
// RenderPass::setViewport (plan.md §3.5) rather than by negating y in the
// shader or the projection. A negative height flips the sign of the signed area
// the rasterizer computes in framebuffer coordinates, which puts the
// clip-space-y-up convention CullMode documents back where Metal and D3D12 have
// it - so CounterClockwise is VK_FRONT_FACE_COUNTER_CLOCKWISE, exactly as it is
// MTLWindingCounterClockwise and FrontCounterClockwise = TRUE. CullModeTests is
// the arbiter, as it was when the D3D12 backend got this wrong by reasoning.
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

// Vulkan spells both pairs out - INCREMENT_AND_CLAMP against
// INCREMENT_AND_WRAP - so there is nothing to get backwards here, which is the
// opposite of the trap D3D12 sets (its INCR wraps and its INCR_SAT clamps, the
// reverse of how the names read; see RenderPipeline-Windows.cpp). Mapped from
// the eacp enum's meaning either way, which is what makes both files checkable
// against the header rather than against each other.
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

// One read/write mask pair applied to both faces, which is what the header
// offers and what D3D12 can express. The reference is dynamic state and is left
// zero here; the pass sets it.
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

// One path for the named modes and for a written-out equation, because
// blendStateFor turns the first into the second.
//
// No alpha-slot substitution, unlike the D3D12 file: Vulkan takes the four
// *_COLOR factors in srcAlphaBlendFactor and dstAlphaBlendFactor and computes
// what the name means, the way Metal does, so a BlendState reaches this backend
// as written.
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

// Culling is baked into the pipeline here, as it is on D3D12 and unlike Metal,
// where it is encoder state the render pass sets from the pipeline it is
// binding.
//
// Vulkan can do it either way - VK_DYNAMIC_STATE_CULL_MODE and
// VK_DYNAMIC_STATE_FRONT_FACE are core 1.3 - and baking is the simpler of the
// two because nothing ever overrides them: RenderPass has no setCullMode and no
// setFrontFace, the descriptor is the only place either value comes from, and a
// pipeline is the only thing that carries one. Two vkCmdSet calls per
// setPipeline would compute the same rasterizer state from the same fields one
// bind later. If a per-pass override is ever added, this is the line that
// changes, along with the dynamic-state list below.
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

        // Refused rather than silently dropped to one sample, which is what a
        // pipeline built against a count the device cannot render would be. A
        // texture asked for the same count answers the same way, so a caller
        // that checked the device gets a consistent story from both.
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

    // Deferred for the reason the compute pipeline is: a command buffer that
    // bound this pipeline may still be open or in flight, and a renderer
    // constructed inside render() is destroyed exactly there.
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

        // Both are dynamic, so the counts are all this carries and the pointers
        // stay null - the pass sets the real rectangles, which is where the
        // negative viewport height lives.
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

        // What a VkRenderPass would otherwise have said, and the reason there
        // is none. Both formats come from depthAttachmentFormat so that the
        // pipeline, the image the pass renders into, its view and the barrier
        // that transitions it all name one value - four places that a draw is
        // rejected for disagreeing on.
        //
        // The depth format is set whenever *either* plane is asked for, which is
        // what both other backends do (RenderPipeline-Apple.mm's `depth ||
        // stencil`, and the D3D12 DSVFormat line) and is not the same as
        // `descriptor.depth`. The two planes are one attachment: a pipeline that
        // paints a stencil mask with the depth test off still draws into a
        // depth-stencil image, and under dynamic rendering the format it
        // declares has to match the depth attachment the pass bound or the draw
        // is refused. Whether the *test* runs is depthTestEnable's business, set
        // from descriptor.depth alone in makeDepthStencilState.
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
                                      VK_NULL_HANDLE,
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

// Both are inside the VkPipeline here, as they are inside the PSO on D3D12.
// Reported anyway so the class reads the same on every backend, and so a caller
// can ask a pipeline what it does.
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
    // Depth state is baked into the pipeline here, so this answers what D3D12
    // answers: the same object, or null when the pipeline has no depth
    // attachment. The separate handle exists for Metal, which binds a
    // MTLDepthStencilState of its own.
    return impl->state.depth ? const_cast<VulkanRenderPipeline*>(&impl->state)
                             : nullptr;
}
} // namespace eacp::GPU
