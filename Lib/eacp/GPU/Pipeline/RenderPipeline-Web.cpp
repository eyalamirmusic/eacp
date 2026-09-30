#include "RenderPipeline.h"

#include "../Device/Device.h"
#include "../Shader/ShaderLibrary.h"
#include "../WebGPU/WebGPUTypes.h"

#include <algorithm>

namespace eacp::GPU
{
namespace
{
WGPUPrimitiveTopology toWebTopology(PrimitiveTopology topology)
{
    switch (topology)
    {
        case PrimitiveTopology::Triangles:
            return WGPUPrimitiveTopology_TriangleList;
        case PrimitiveTopology::TriangleStrip:
            return WGPUPrimitiveTopology_TriangleStrip;
        case PrimitiveTopology::Lines:
            return WGPUPrimitiveTopology_LineList;
        case PrimitiveTopology::LineStrip:
            return WGPUPrimitiveTopology_LineStrip;
        case PrimitiveTopology::Points:
            return WGPUPrimitiveTopology_PointList;
    }

    return WGPUPrimitiveTopology_TriangleList;
}

WGPUVertexFormat toWebVertexFormat(VertexFormat format)
{
    switch (format)
    {
        case VertexFormat::Float:
            return WGPUVertexFormat_Float32;
        case VertexFormat::Float2:
            return WGPUVertexFormat_Float32x2;
        case VertexFormat::Float3:
            return WGPUVertexFormat_Float32x3;
        case VertexFormat::Float4:
            return WGPUVertexFormat_Float32x4;
        case VertexFormat::UByte4Norm:
            return WGPUVertexFormat_Unorm8x4;
        case VertexFormat::Half2:
            return WGPUVertexFormat_Float16x2;
        case VertexFormat::Half4:
            return WGPUVertexFormat_Float16x4;
        case VertexFormat::Short2Norm:
            return WGPUVertexFormat_Snorm16x2;
        case VertexFormat::Short4Norm:
            return WGPUVertexFormat_Snorm16x4;
    }

    return WGPUVertexFormat_Float32x3;
}

WGPUCullMode toWebCullMode(CullMode mode)
{
    switch (mode)
    {
        case CullMode::None:
            return WGPUCullMode_None;
        case CullMode::Front:
            return WGPUCullMode_Front;
        case CullMode::Back:
            return WGPUCullMode_Back;
    }

    return WGPUCullMode_None;
}

// WebGPU winds as Metal does - clip-space y up, framebuffer y down - so the
// convention is spelled the same way as MTLWindingCounterClockwise.
WGPUFrontFace toWebFrontFace(Winding winding)
{
    return winding == Winding::CounterClockwise ? WGPUFrontFace_CCW
                                                : WGPUFrontFace_CW;
}

WGPUCompareFunction toWebCompare(CompareFunction compare)
{
    switch (compare)
    {
        case CompareFunction::Never:
            return WGPUCompareFunction_Never;
        case CompareFunction::Less:
            return WGPUCompareFunction_Less;
        case CompareFunction::LessEqual:
            return WGPUCompareFunction_LessEqual;
        case CompareFunction::Equal:
            return WGPUCompareFunction_Equal;
        case CompareFunction::NotEqual:
            return WGPUCompareFunction_NotEqual;
        case CompareFunction::GreaterEqual:
            return WGPUCompareFunction_GreaterEqual;
        case CompareFunction::Greater:
            return WGPUCompareFunction_Greater;
        case CompareFunction::Always:
            return WGPUCompareFunction_Always;
    }

    return WGPUCompareFunction_LessEqual;
}

WGPUStencilOperation toWebStencilOp(StencilOp op)
{
    switch (op)
    {
        case StencilOp::Keep:
            return WGPUStencilOperation_Keep;
        case StencilOp::Zero:
            return WGPUStencilOperation_Zero;
        case StencilOp::Replace:
            return WGPUStencilOperation_Replace;
        case StencilOp::IncrementClamp:
            return WGPUStencilOperation_IncrementClamp;
        case StencilOp::DecrementClamp:
            return WGPUStencilOperation_DecrementClamp;
        case StencilOp::Invert:
            return WGPUStencilOperation_Invert;
        case StencilOp::IncrementWrap:
            return WGPUStencilOperation_IncrementWrap;
        case StencilOp::DecrementWrap:
            return WGPUStencilOperation_DecrementWrap;
    }

    return WGPUStencilOperation_Keep;
}

WGPUStencilFaceState toWebStencilFace(const StencilFace& face)
{
    auto state = WGPUStencilFaceState {};
    state.compare = toWebCompare(face.compare);
    state.failOp = toWebStencilOp(face.stencilFail);
    state.depthFailOp = toWebStencilOp(face.depthFail);
    state.passOp = toWebStencilOp(face.pass);

    return state;
}

WGPUStencilFaceState passingStencilFace()
{
    return toWebStencilFace(StencilFace {});
}

WGPUBlendFactor toWebBlendFactor(BlendFactor factor)
{
    switch (factor)
    {
        case BlendFactor::Zero:
            return WGPUBlendFactor_Zero;
        case BlendFactor::One:
            return WGPUBlendFactor_One;
        case BlendFactor::SourceColor:
            return WGPUBlendFactor_Src;
        case BlendFactor::OneMinusSourceColor:
            return WGPUBlendFactor_OneMinusSrc;
        case BlendFactor::SourceAlpha:
            return WGPUBlendFactor_SrcAlpha;
        case BlendFactor::OneMinusSourceAlpha:
            return WGPUBlendFactor_OneMinusSrcAlpha;
        case BlendFactor::DestinationColor:
            return WGPUBlendFactor_Dst;
        case BlendFactor::OneMinusDestinationColor:
            return WGPUBlendFactor_OneMinusDst;
        case BlendFactor::DestinationAlpha:
            return WGPUBlendFactor_DstAlpha;
        case BlendFactor::OneMinusDestinationAlpha:
            return WGPUBlendFactor_OneMinusDstAlpha;
        case BlendFactor::SourceAlphaSaturated:
            return WGPUBlendFactor_SrcAlphaSaturated;
    }

    return WGPUBlendFactor_One;
}

WGPUBlendOperation toWebBlendOperation(BlendOperation operation)
{
    switch (operation)
    {
        case BlendOperation::Add:
            return WGPUBlendOperation_Add;
        case BlendOperation::Subtract:
            return WGPUBlendOperation_Subtract;
        case BlendOperation::ReverseSubtract:
            return WGPUBlendOperation_ReverseSubtract;
        case BlendOperation::Min:
            return WGPUBlendOperation_Min;
        case BlendOperation::Max:
            return WGPUBlendOperation_Max;
    }

    return WGPUBlendOperation_Add;
}

// Min and max ignore their factors on Metal and D3D12; WebGPU requires both to
// be One, which is the same arithmetic.
WGPUBlendComponent toWebBlendComponent(BlendFactor source,
                                       BlendFactor destination,
                                       BlendOperation operation)
{
    auto component = WGPUBlendComponent {};
    component.operation = toWebBlendOperation(operation);

    const auto ignoresFactors =
        operation == BlendOperation::Min || operation == BlendOperation::Max;

    component.srcFactor =
        ignoresFactors ? WGPUBlendFactor_One : toWebBlendFactor(source);
    component.dstFactor =
        ignoresFactors ? WGPUBlendFactor_One : toWebBlendFactor(destination);

    return component;
}

WGPUColorWriteMask toWebWriteMask(const ColorWriteMask& mask)
{
    auto value = WGPUColorWriteMask_None;

    if (mask.red)
        value |= WGPUColorWriteMask_Red;
    if (mask.green)
        value |= WGPUColorWriteMask_Green;
    if (mask.blue)
        value |= WGPUColorWriteMask_Blue;
    if (mask.alpha)
        value |= WGPUColorWriteMask_Alpha;

    return value;
}

int strideForSlot(const VertexLayout& layout, int slot)
{
    if (!layout.buffers.empty())
        return slot < layout.buffers.size() ? layout.buffers[slot].stride : 0;

    return layout.stride;
}

StepRate stepRateForSlot(const VertexLayout& layout, int slot)
{
    if (slot >= 0 && slot < layout.buffers.size())
        return layout.buffers[slot].stepRate;

    return StepRate::PerVertex;
}

// Attribute i takes location i, which is what the WGSL emitter prints; a slot
// no attribute names is left out of the pipeline, and needs no buffer bound.
struct WebVertexInput
{
    bool build(const VertexLayout& layout)
    {
        auto slots = 0;

        for (const auto& attribute: layout.attributes)
        {
            if (attribute.bufferIndex < 0)
                return false;

            slots = std::max(slots, attribute.bufferIndex + 1);
        }

        const auto& limits = getWebGPUShared().getLimits();

        if (layout.attributes.size() > (int) limits.maxVertexAttributes)
        {
            LOG("WebGPU: a pipeline with ",
                layout.attributes.size(),
                " vertex attributes; this device allows ",
                limits.maxVertexAttributes);
            return false;
        }

        if (slots > (int) limits.maxVertexBuffers)
        {
            LOG("WebGPU: a pipeline reading ",
                slots,
                " vertex buffers; this device allows ",
                limits.maxVertexBuffers);
            return false;
        }

        attributes.resize(slots);

        for (auto index = 0; index < layout.attributes.size(); ++index)
        {
            const auto& attribute = layout.attributes[index];

            auto entry = WGPU_VERTEX_ATTRIBUTE_INIT;
            entry.format = toWebVertexFormat(attribute.format);
            entry.offset = static_cast<std::uint64_t>(attribute.offset);
            entry.shaderLocation = static_cast<std::uint32_t>(index);

            attributes[attribute.bufferIndex].add(entry);
        }

        for (auto slot = 0; slot < slots; ++slot)
        {
            auto buffer = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
            const auto& slotAttributes = attributes[slot];

            if (!slotAttributes.empty())
            {
                buffer.stepMode =
                    stepRateForSlot(layout, slot) == StepRate::PerInstance
                        ? WGPUVertexStepMode_Instance
                        : WGPUVertexStepMode_Vertex;
                buffer.arrayStride =
                    static_cast<std::uint64_t>(strideForSlot(layout, slot));
                buffer.attributeCount =
                    static_cast<std::size_t>(slotAttributes.size());
                buffer.attributes = slotAttributes.data();
            }

            buffers.add(buffer);
        }

        return true;
    }

    Vector<Vector<WGPUVertexAttribute>> attributes;
    Vector<WGPUVertexBufferLayout> buffers;
};

void applyDepthStencil(WGPUDepthStencilState& state,
                       const RenderPipelineDescriptor& from,
                       WGPUTextureFormat format)
{
    state.format = format;

    // A pipeline that only masks the stencil still has the depth plane under
    // it, and must not disturb it.
    state.depthWriteEnabled = from.depth && from.depthWrite ? WGPUOptionalBool_True
                                                            : WGPUOptionalBool_False;
    state.depthCompare =
        from.depth ? toWebCompare(from.depthCompare) : WGPUCompareFunction_Always;

    if (from.stencil && webFormatHasStencil(format))
    {
        state.stencilFront = toWebStencilFace(from.stencilFront);
        state.stencilBack = toWebStencilFace(from.stencilBack);
        state.stencilReadMask = from.stencilReadMask;
        state.stencilWriteMask = from.stencilWriteMask;
        return;
    }

    state.stencilFront = passingStencilFace();
    state.stencilBack = passingStencilFace();
}
} // namespace

WGPURenderPipeline buildWebRenderPipeline(const WebRenderPipeline& state,
                                          WGPUTextureFormat depthFormat)
{
    const auto& from = state.descriptor;

    auto input = WebVertexInput {};

    if (!input.build(from.vertexLayout))
        return nullptr;

    auto descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    descriptor.label = toWebString("eacp render pipeline");
    descriptor.layout = state.pipelineLayout;

    descriptor.vertex.module = state.module;
    descriptor.vertex.entryPoint = toWebString(state.vertexEntry);
    descriptor.vertex.bufferCount = static_cast<std::size_t>(input.buffers.size());
    descriptor.vertex.buffers = input.buffers.data();

    descriptor.primitive.topology = toWebTopology(from.topology);
    descriptor.primitive.frontFace = toWebFrontFace(from.frontFace);
    descriptor.primitive.cullMode = toWebCullMode(from.cullMode);

    auto depthStencil = WGPU_DEPTH_STENCIL_STATE_INIT;

    if (depthFormat != WGPUTextureFormat_Undefined)
    {
        applyDepthStencil(depthStencil, from, depthFormat);
        descriptor.depthStencil = &depthStencil;
    }

    descriptor.multisample.count =
        static_cast<std::uint32_t>(from.sampleCount > 1 ? from.sampleCount : 1);

    const auto blendSource =
        from.blend ? *from.blend : blendStateFor(from.blendMode);

    auto blend = WGPU_BLEND_STATE_INIT;
    blend.color = toWebBlendComponent(blendSource.sourceColor,
                                      blendSource.destinationColor,
                                      blendSource.colorOperation);
    blend.alpha = toWebBlendComponent(blendSource.sourceAlpha,
                                      blendSource.destinationAlpha,
                                      blendSource.alphaOperation);

    auto target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = toWebFormat(from.colorFormat);
    target.blend = blendSource.enabled ? &blend : nullptr;
    target.writeMask = toWebWriteMask(from.colorWriteMask);

    auto fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = state.module;
    fragment.entryPoint = toWebString(state.fragmentEntry);
    fragment.targetCount = 1;
    fragment.targets = &target;

    descriptor.fragment = &fragment;

    pushWebErrorScope();

    auto pipeline =
        wgpuDeviceCreateRenderPipeline(getWebGPUShared().getDevice(), &descriptor);

    popWebErrorScope("a render pipeline (" + state.vertexEntry + " / "
                     + state.fragmentEntry + ")");

    return pipeline;
}

WebRenderPipeline::~WebRenderPipeline()
{
    for (auto& variant: variants)
        if (variant.pipeline != nullptr)
            wgpuRenderPipelineRelease(variant.pipeline);

    if (pipeline != nullptr)
        wgpuRenderPipelineRelease(pipeline);

    if (pipelineLayout != nullptr)
        wgpuPipelineLayoutRelease(pipelineLayout);

    if (groupLayout != nullptr)
        wgpuBindGroupLayoutRelease(groupLayout);

    if (module != nullptr)
        wgpuShaderModuleRelease(module);
}

WGPURenderPipeline WebRenderPipeline::forDepthFormat(WGPUTextureFormat passDepth)
{
    if (passDepth == depthFormat || pipeline == nullptr)
        return pipeline;

    for (const auto& variant: variants)
        if (variant.depthFormat == passDepth)
            return variant.pipeline;

    auto variant = Variant {};
    variant.depthFormat = passDepth;
    variant.pipeline = buildWebRenderPipeline(*this, passDepth);

    variants.add(variant);
    return variant.pipeline;
}

struct RenderPipeline::Native
{
    Native(Device& device, const RenderPipelineDescriptor& descriptor)
        : topology(descriptor.topology)
        , cullMode(descriptor.cullMode)
        , frontFace(descriptor.frontFace)
    {
        if (!device.isValid() || descriptor.library == nullptr)
            return;

        const auto sampleCount =
            descriptor.sampleCount > 1 ? descriptor.sampleCount : 1;

        if (!device.supportsSampleCount(sampleCount))
        {
            LOG("WebGPU: no render pipeline at ",
                sampleCount,
                "x MSAA - WebGPU has 1 and 4");
            return;
        }

        auto* program =
            static_cast<WebShaderProgram*>(descriptor.library->nativeLibrary());

        if (program == nullptr || !program->isValid())
            return;

        state.module = program->module;
        state.groupLayout = program->groupLayout;
        state.pipelineLayout = program->pipelineLayout;

        wgpuShaderModuleAddRef(state.module);
        wgpuBindGroupLayoutAddRef(state.groupLayout);
        wgpuPipelineLayoutAddRef(state.pipelineLayout);

        state.bindings = program->bindings;
        state.vertexEntry = descriptor.library->vertexEntry();
        state.fragmentEntry = descriptor.library->fragmentEntry();
        state.descriptor = descriptor;
        state.descriptor.library = nullptr;

        state.depthFormat = descriptor.depth || descriptor.stencil
                                ? webDepthFormat(descriptor.stencil)
                                : WGPUTextureFormat_Undefined;

        state.pipeline = buildWebRenderPipeline(state, state.depthFormat);
    }

    PrimitiveTopology topology = PrimitiveTopology::Triangles;
    CullMode cullMode = CullMode::None;
    Winding frontFace = Winding::CounterClockwise;
    mutable WebRenderPipeline state;
};

RenderPipeline::RenderPipeline(Device& device,
                               const RenderPipelineDescriptor& descriptor)
    : impl(device, descriptor)
{
}

bool RenderPipeline::isValid() const
{
    return impl->state.isValid();
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
    return &impl->state;
}

void* RenderPipeline::nativeDepthState() const
{
    const auto tests = impl->state.depthFormat != WGPUTextureFormat_Undefined;

    return tests ? &impl->state : nullptr;
}
} // namespace eacp::GPU
