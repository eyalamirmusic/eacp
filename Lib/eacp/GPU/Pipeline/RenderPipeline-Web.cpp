#include "RenderPipeline.h"

#include "../Device/Device.h"
#include "../Shader/ShaderLibrary.h"
#include "../WebGPU/WebGPUContext.h"

#include <eacp/Core/Utils/Logging.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace eacp::GPU
{
namespace
{
// A pipeline with no fragment stage still has to name the pass's colour
// formats, which only a fragment stage can carry: this one writes nothing.
constexpr auto emptyFragmentSource = std::string_view {"@fragment fn main() {}\n"};

wgpu::PrimitiveTopology toWGPUTopology(PrimitiveTopology topology)
{
    switch (topology)
    {
        case PrimitiveTopology::TriangleStrip:
            return wgpu::PrimitiveTopology::TriangleStrip;
        case PrimitiveTopology::Lines:
            return wgpu::PrimitiveTopology::LineList;
        case PrimitiveTopology::LineStrip:
            return wgpu::PrimitiveTopology::LineStrip;
        case PrimitiveTopology::Points:
            return wgpu::PrimitiveTopology::PointList;
        default:
            return wgpu::PrimitiveTopology::TriangleList;
    }
}

wgpu::VertexFormat toWGPUVertexFormat(VertexFormat format)
{
    switch (format)
    {
        case VertexFormat::Float:
            return wgpu::VertexFormat::Float32;
        case VertexFormat::Float2:
            return wgpu::VertexFormat::Float32x2;
        case VertexFormat::Float3:
            return wgpu::VertexFormat::Float32x3;
        case VertexFormat::Float4:
            return wgpu::VertexFormat::Float32x4;
        case VertexFormat::UByte4Norm:
            return wgpu::VertexFormat::Unorm8x4;
        case VertexFormat::Half2:
            return wgpu::VertexFormat::Float16x2;
        case VertexFormat::Half4:
            return wgpu::VertexFormat::Float16x4;
        case VertexFormat::Short2Norm:
            return wgpu::VertexFormat::Snorm16x2;
        case VertexFormat::Short4Norm:
            return wgpu::VertexFormat::Snorm16x4;
        case VertexFormat::UByte:
            return wgpu::VertexFormat::Uint8;
        case VertexFormat::UByte2:
            return wgpu::VertexFormat::Uint8x2;
        case VertexFormat::UByte4:
            return wgpu::VertexFormat::Uint8x4;
        case VertexFormat::UByteNorm:
            return wgpu::VertexFormat::Unorm8;
        case VertexFormat::UByte2Norm:
            return wgpu::VertexFormat::Unorm8x2;
        case VertexFormat::Byte:
            return wgpu::VertexFormat::Sint8;
        case VertexFormat::Byte2:
            return wgpu::VertexFormat::Sint8x2;
        case VertexFormat::Byte4:
            return wgpu::VertexFormat::Sint8x4;
        case VertexFormat::ByteNorm:
            return wgpu::VertexFormat::Snorm8;
        case VertexFormat::Byte2Norm:
            return wgpu::VertexFormat::Snorm8x2;
        case VertexFormat::Byte4Norm:
            return wgpu::VertexFormat::Snorm8x4;
        case VertexFormat::UShort:
            return wgpu::VertexFormat::Uint16;
        case VertexFormat::UShort2:
            return wgpu::VertexFormat::Uint16x2;
        case VertexFormat::UShort4:
            return wgpu::VertexFormat::Uint16x4;
        case VertexFormat::UShortNorm:
            return wgpu::VertexFormat::Unorm16;
        case VertexFormat::UShort2Norm:
            return wgpu::VertexFormat::Unorm16x2;
        case VertexFormat::UShort4Norm:
            return wgpu::VertexFormat::Unorm16x4;
        case VertexFormat::Short:
            return wgpu::VertexFormat::Sint16;
        case VertexFormat::Short2:
            return wgpu::VertexFormat::Sint16x2;
        case VertexFormat::Short4:
            return wgpu::VertexFormat::Sint16x4;
        case VertexFormat::ShortNorm:
            return wgpu::VertexFormat::Snorm16;
        case VertexFormat::Half:
            return wgpu::VertexFormat::Float16;
        case VertexFormat::UInt1010102Norm:
            return wgpu::VertexFormat::Unorm10_10_10_2;
        case VertexFormat::UInt:
            return wgpu::VertexFormat::Uint32;
        case VertexFormat::UInt2:
            return wgpu::VertexFormat::Uint32x2;
        case VertexFormat::UInt3:
            return wgpu::VertexFormat::Uint32x3;
        case VertexFormat::UInt4:
            return wgpu::VertexFormat::Uint32x4;
        case VertexFormat::Int:
            return wgpu::VertexFormat::Sint32;
        case VertexFormat::Int2:
            return wgpu::VertexFormat::Sint32x2;
        case VertexFormat::Int3:
            return wgpu::VertexFormat::Sint32x3;
        case VertexFormat::Int4:
            return wgpu::VertexFormat::Sint32x4;
    }

    return wgpu::VertexFormat::Float32x4;
}

wgpu::CullMode toWGPUCullMode(CullMode mode)
{
    switch (mode)
    {
        case CullMode::Front:
            return wgpu::CullMode::Front;
        case CullMode::Back:
            return wgpu::CullMode::Back;
        default:
            return wgpu::CullMode::None;
    }
}

wgpu::StencilOperation toWGPUStencilOp(StencilOp op)
{
    switch (op)
    {
        case StencilOp::Zero:
            return wgpu::StencilOperation::Zero;
        case StencilOp::Replace:
            return wgpu::StencilOperation::Replace;
        case StencilOp::IncrementClamp:
            return wgpu::StencilOperation::IncrementClamp;
        case StencilOp::DecrementClamp:
            return wgpu::StencilOperation::DecrementClamp;
        case StencilOp::Invert:
            return wgpu::StencilOperation::Invert;
        case StencilOp::IncrementWrap:
            return wgpu::StencilOperation::IncrementWrap;
        case StencilOp::DecrementWrap:
            return wgpu::StencilOperation::DecrementWrap;
        default:
            return wgpu::StencilOperation::Keep;
    }
}

wgpu::StencilFaceState toWGPUStencilFace(const StencilFace& face)
{
    auto state = wgpu::StencilFaceState {};
    state.compare = toWGPUCompare(face.compare);
    state.failOp = toWGPUStencilOp(face.stencilFail);
    state.depthFailOp = toWGPUStencilOp(face.depthFail);
    state.passOp = toWGPUStencilOp(face.pass);
    return state;
}

wgpu::BlendFactor toWGPUBlendFactor(BlendFactor factor)
{
    switch (factor)
    {
        case BlendFactor::Zero:
            return wgpu::BlendFactor::Zero;
        case BlendFactor::One:
            return wgpu::BlendFactor::One;
        case BlendFactor::SourceColor:
            return wgpu::BlendFactor::Src;
        case BlendFactor::OneMinusSourceColor:
            return wgpu::BlendFactor::OneMinusSrc;
        case BlendFactor::SourceAlpha:
            return wgpu::BlendFactor::SrcAlpha;
        case BlendFactor::OneMinusSourceAlpha:
            return wgpu::BlendFactor::OneMinusSrcAlpha;
        case BlendFactor::DestinationColor:
            return wgpu::BlendFactor::Dst;
        case BlendFactor::OneMinusDestinationColor:
            return wgpu::BlendFactor::OneMinusDst;
        case BlendFactor::DestinationAlpha:
            return wgpu::BlendFactor::DstAlpha;
        case BlendFactor::OneMinusDestinationAlpha:
            return wgpu::BlendFactor::OneMinusDstAlpha;
        case BlendFactor::SourceAlphaSaturated:
            return wgpu::BlendFactor::SrcAlphaSaturated;
    }

    return wgpu::BlendFactor::One;
}

wgpu::BlendOperation toWGPUBlendOperation(BlendOperation operation)
{
    switch (operation)
    {
        case BlendOperation::Subtract:
            return wgpu::BlendOperation::Subtract;
        case BlendOperation::ReverseSubtract:
            return wgpu::BlendOperation::ReverseSubtract;
        case BlendOperation::Min:
            return wgpu::BlendOperation::Min;
        case BlendOperation::Max:
            return wgpu::BlendOperation::Max;
        default:
            return wgpu::BlendOperation::Add;
    }
}

// WebGPU takes One/One for both factors of a min or max, which ignores them.
wgpu::BlendComponent toWGPUBlendComponent(BlendFactor source,
                                          BlendFactor destination,
                                          BlendOperation operation)
{
    auto component = wgpu::BlendComponent {};
    component.operation = toWGPUBlendOperation(operation);

    const auto ignoresFactors =
        operation == BlendOperation::Min || operation == BlendOperation::Max;

    component.srcFactor =
        ignoresFactors ? wgpu::BlendFactor::One : toWGPUBlendFactor(source);
    component.dstFactor =
        ignoresFactors ? wgpu::BlendFactor::One : toWGPUBlendFactor(destination);
    return component;
}

wgpu::ColorWriteMask toWGPUWriteMask(const ColorWriteMask& mask)
{
    auto result = wgpu::ColorWriteMask::None;

    if (mask.red)
        result |= wgpu::ColorWriteMask::Red;

    if (mask.green)
        result |= wgpu::ColorWriteMask::Green;

    if (mask.blue)
        result |= wgpu::ColorWriteMask::Blue;

    if (mask.alpha)
        result |= wgpu::ColorWriteMask::Alpha;

    return result;
}

// Everything a pipeline descriptor points into, kept alive while one is built.
struct PipelineParts
{
    Vector<wgpu::VertexBufferLayout> buffers;
    Vector<Vector<wgpu::VertexAttribute>> attributes;
    Vector<wgpu::ColorTargetState> targets;
    Vector<wgpu::BlendState> blends;
};

void describeVertexBuffers(const VertexLayout& layout, PipelineParts& parts)
{
    const auto slotCount =
        layout.buffers.empty() ? 1 : static_cast<int>(layout.buffers.size());

    parts.attributes.resize(slotCount);

    for (auto i = 0; i < layout.attributes.size(); ++i)
    {
        const auto& from = layout.attributes[i];
        const auto slot = std::clamp(from.bufferIndex, 0, slotCount - 1);

        auto& attribute = parts.attributes[slot].create();
        attribute.format = toWGPUVertexFormat(from.format);
        attribute.offset = static_cast<std::uint64_t>(from.offset);
        attribute.shaderLocation = static_cast<std::uint32_t>(i);
    }

    for (auto slot = 0; slot < slotCount; ++slot)
    {
        const auto& attributes = parts.attributes[slot];
        auto& buffer = parts.buffers.create();

        if (layout.buffers.empty())
        {
            buffer.arrayStride = static_cast<std::uint64_t>(layout.stride);
        }
        else
        {
            const auto& from = layout.buffers[slot];
            buffer.arrayStride = static_cast<std::uint64_t>(from.stride);
            buffer.stepMode = from.stepRate == StepRate::PerInstance
                                  ? wgpu::VertexStepMode::Instance
                                  : wgpu::VertexStepMode::Vertex;
        }

        buffer.attributeCount = attributes.getSize();
        buffer.attributes = attributes.data();

        if (attributes.empty())
            buffer.stepMode = wgpu::VertexStepMode::Undefined;
    }

    if (layout.attributes.empty())
        parts.buffers.clear();
}

void describeColorTargets(const RenderPipelineDescriptor& descriptor,
                          bool hasFragment,
                          PipelineParts& parts)
{
    const auto count = descriptor.colorTargetCount();
    parts.blends.reserve(count);

    for (auto index = 0; index < count; ++index)
    {
        const auto target = writtenColorTarget(descriptor, index, hasFragment);
        const auto blend = resolvedBlend(target);

        auto& state = parts.targets.create();
        state.format = toWGPUFormat(target.format);
        state.writeMask = toWGPUWriteMask(target.writeMask);

        if (blend.enabled)
        {
            auto& blendState = parts.blends.create();
            blendState.color = toWGPUBlendComponent(
                blend.sourceColor, blend.destinationColor, blend.colorOperation);
            blendState.alpha = toWGPUBlendComponent(
                blend.sourceAlpha, blend.destinationAlpha, blend.alphaOperation);
            state.blend = &blendState;
        }
    }
}

wgpu::DepthStencilState
    describeDepthStencil(const RenderPipelineDescriptor& descriptor,
                         wgpu::TextureFormat depthFormat)
{
    auto state = wgpu::DepthStencilState {};
    state.format = depthFormat;
    state.depthWriteEnabled = descriptor.depth && descriptor.depthWrite
                                  ? wgpu::OptionalBool::True
                                  : wgpu::OptionalBool::False;
    state.depthCompare = descriptor.depth ? toWGPUCompare(descriptor.depthCompare)
                                          : wgpu::CompareFunction::Always;

    const auto hasStencil =
        depthFormat == wgpu::TextureFormat::Depth24PlusStencil8
        || depthFormat == wgpu::TextureFormat::Depth32FloatStencil8;

    if (descriptor.stencil && hasStencil)
    {
        state.stencilFront = toWGPUStencilFace(descriptor.stencilFront);
        state.stencilBack = toWGPUStencilFace(descriptor.stencilBack);
        state.stencilReadMask = descriptor.stencilReadMask;
        state.stencilWriteMask = descriptor.stencilWriteMask;
    }

    const auto bias = appliedDepthBias(descriptor);
    state.depthBias = static_cast<std::int32_t>(std::lround(bias.constant));
    state.depthBiasSlopeScale = bias.slopeScale;
    state.depthBiasClamp = bias.clamp;
    return state;
}
} // namespace

struct RenderPipeline::Native
{
    Native(Device& device, const RenderPipelineDescriptor& descriptorToUse)
        : descriptor(descriptorToUse)
        , depthBias(appliedDepthBias(descriptorToUse))
    {
        auto& context = getWebGPUContext(device);
        const auto* library = descriptor.library;

        if (!context.isValid() || library == nullptr || !library->isValid())
            return;

        auto* compiled = static_cast<WebGPUShaderProgram*>(library->nativeLibrary());

        if (compiled == nullptr || compiled->module == nullptr)
            return;

        program = *compiled;
        vertexEntry = library->vertexEntry();
        fragmentEntry = library->fragmentEntry();
        hasFragment = library->hasFragment();
        descriptor.library = nullptr;

        state.layout = context.makeBindingLayout(program.bindings, false);

        if (!hasFragment)
            emptyFragment = makeEmptyFragment(context);

        auto* contextPointer = &context;

        state.build = [this, contextPointer](wgpu::TextureFormat depthFormat,
                                             const WebGPUBindingLayout& layout)
        { return build(*contextPointer, depthFormat, layout); };

        state.makeLayout = [this, contextPointer](const Vector<int>& unfilterable)
        {
            return contextPointer->makeBindingLayout(
                withUnfilterableTextures(
                    program.bindings, unfilterable, program.source),
                false);
        };

        const auto declaredDepth =
            descriptor.stencil ? wgpu::TextureFormat::Depth24PlusStencil8
            : descriptor.depth ? wgpu::TextureFormat::Depth32Float
                               : wgpu::TextureFormat::Undefined;

        state.valid = state.pipelineFor(declaredDepth) != nullptr;
    }

    static wgpu::ShaderModule makeEmptyFragment(WebGPUContext& context)
    {
        auto wgsl = wgpu::ShaderSourceWGSL {};
        wgsl.code = wgpu::StringView {emptyFragmentSource.data(),
                                      emptyFragmentSource.size()};

        auto descriptor = wgpu::ShaderModuleDescriptor {};
        descriptor.nextInChain = &wgsl;
        return context.getDevice().CreateShaderModule(&descriptor);
    }

    wgpu::RenderPipeline build(WebGPUContext& context,
                               wgpu::TextureFormat depthFormat,
                               const WebGPUBindingLayout& layout)
    {
        auto parts = PipelineParts {};
        describeVertexBuffers(descriptor.vertexLayout, parts);
        describeColorTargets(descriptor, hasFragment, parts);

        const auto constants = WebGPUConstantEntries {descriptor.constants};

        auto pipeline = wgpu::RenderPipelineDescriptor {};
        pipeline.layout = layout.pipeline;

        pipeline.vertex.module = program.module;
        pipeline.vertex.entryPoint =
            wgpu::StringView {vertexEntry.data(), vertexEntry.size()};
        pipeline.vertex.bufferCount = parts.buffers.getSize();
        pipeline.vertex.buffers = parts.buffers.data();
        pipeline.vertex.constantCount = constants.entries.getSize();
        pipeline.vertex.constants = constants.entries.data();

        pipeline.primitive.topology = toWGPUTopology(descriptor.topology);
        pipeline.primitive.cullMode = toWGPUCullMode(descriptor.cullMode);
        pipeline.primitive.frontFace = descriptor.frontFace == Winding::Clockwise
                                           ? wgpu::FrontFace::CW
                                           : wgpu::FrontFace::CCW;

        auto depthStencil = describeDepthStencil(descriptor, depthFormat);

        if (depthFormat != wgpu::TextureFormat::Undefined)
            pipeline.depthStencil = &depthStencil;

        pipeline.multisample.count =
            static_cast<std::uint32_t>(std::max(1, descriptor.sampleCount));

        auto fragment = wgpu::FragmentState {};
        fragment.module = hasFragment ? program.module : emptyFragment;
        fragment.entryPoint = hasFragment ? wgpu::StringView {fragmentEntry.data(),
                                                              fragmentEntry.size()}
                                          : wgpu::StringView {"main", 4};
        fragment.targetCount = parts.targets.getSize();
        fragment.targets = parts.targets.data();

        if (hasFragment)
        {
            fragment.constantCount = constants.entries.getSize();
            fragment.constants = constants.entries.data();
        }

        pipeline.fragment = &fragment;

        auto message = std::string {};
        auto result = wgpu::RenderPipeline {};

        const auto built = context.succeeds(
            [&] { result = context.getDevice().CreateRenderPipeline(&pipeline); },
            &message);

        if (!built)
        {
            LOG("WebGPU: a render pipeline was refused: ", message);
            return {};
        }

        return result;
    }

    // The library a descriptor names may go before the pipeline does, and a
    // variant for a new depth format is built from these after it has.
    RenderPipelineDescriptor descriptor;
    DepthBias depthBias;
    WebGPUShaderProgram program;
    std::string vertexEntry;
    std::string fragmentEntry;
    bool hasFragment = false;
    wgpu::ShaderModule emptyFragment;
    WebGPURenderPipeline state;
};

RenderPipeline::RenderPipeline(Device& device,
                               const RenderPipelineDescriptor& descriptor)
    : impl(device, descriptor)
{
}

bool RenderPipeline::isValid() const
{
    return impl->state.valid;
}

PrimitiveTopology RenderPipeline::topology() const
{
    return impl->descriptor.topology;
}

CullMode RenderPipeline::cullMode() const
{
    return impl->descriptor.cullMode;
}

Winding RenderPipeline::frontFace() const
{
    return impl->descriptor.frontFace;
}

DepthBias RenderPipeline::depthBias() const
{
    return impl->depthBias;
}

void* RenderPipeline::nativeState() const
{
    return const_cast<WebGPURenderPipeline*>(&impl->state);
}

void* RenderPipeline::nativeDepthState() const
{
    return nullptr;
}
} // namespace eacp::GPU
