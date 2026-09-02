#import <Metal/Metal.h>

#include "RenderPipeline.h"

#include "../Device/Device.h"
#include "../Shader/ShaderLibrary.h"

#include <eacp/Core/ObjC/ObjC.h>

namespace eacp::GPU
{
static MTLVertexFormat toMetalVertexFormat(VertexFormat format)
{
    switch (format)
    {
        case VertexFormat::Float:
            return MTLVertexFormatFloat;
        case VertexFormat::Float2:
            return MTLVertexFormatFloat2;
        case VertexFormat::Float3:
            return MTLVertexFormatFloat3;
        case VertexFormat::Float4:
            return MTLVertexFormatFloat4;

        // The Normalized variants, not the plain integer ones: an attribute
        // declared UByte4Norm reads as 0..1 in the shader, where plain
        // MTLVertexFormatUChar4 would hand it 0..255. The same choice on D3D12
        // is UNORM against UINT, and the two backends have to agree on it or a
        // colour comes out 255 times too bright on one of them.
        case VertexFormat::UByte4Norm:
            return MTLVertexFormatUChar4Normalized;
        case VertexFormat::Half2:
            return MTLVertexFormatHalf2;
        case VertexFormat::Half4:
            return MTLVertexFormatHalf4;
        case VertexFormat::Short2Norm:
            return MTLVertexFormatShort2Normalized;
        case VertexFormat::Short4Norm:
            return MTLVertexFormatShort4Normalized;
    }

    return MTLVertexFormatFloat3;
}

static MTLPixelFormat toMetalPixelFormat(PixelFormat format)
{
    switch (format)
    {
        case PixelFormat::BGRA8Unorm:
            return MTLPixelFormatBGRA8Unorm;
        case PixelFormat::RGBA8Unorm:
            return MTLPixelFormatRGBA8Unorm;
        case PixelFormat::RGBA16Float:
            return MTLPixelFormatRGBA16Float;
        case PixelFormat::RGBA32Float:
            return MTLPixelFormatRGBA32Float;
        case PixelFormat::R32Float:
            return MTLPixelFormatR32Float;
    }

    return MTLPixelFormatBGRA8Unorm;
}

static MTLBlendFactor toMetalBlendFactor(BlendFactor factor)
{
    switch (factor)
    {
        case BlendFactor::Zero:
            return MTLBlendFactorZero;
        case BlendFactor::One:
            return MTLBlendFactorOne;
        case BlendFactor::SourceColor:
            return MTLBlendFactorSourceColor;
        case BlendFactor::OneMinusSourceColor:
            return MTLBlendFactorOneMinusSourceColor;
        case BlendFactor::SourceAlpha:
            return MTLBlendFactorSourceAlpha;
        case BlendFactor::OneMinusSourceAlpha:
            return MTLBlendFactorOneMinusSourceAlpha;
        case BlendFactor::DestinationColor:
            return MTLBlendFactorDestinationColor;
        case BlendFactor::OneMinusDestinationColor:
            return MTLBlendFactorOneMinusDestinationColor;
        case BlendFactor::DestinationAlpha:
            return MTLBlendFactorDestinationAlpha;
        case BlendFactor::OneMinusDestinationAlpha:
            return MTLBlendFactorOneMinusDestinationAlpha;
        case BlendFactor::SourceAlphaSaturated:
            return MTLBlendFactorSourceAlphaSaturated;
    }

    return MTLBlendFactorOne;
}

static MTLBlendOperation toMetalBlendOperation(BlendOperation operation)
{
    switch (operation)
    {
        case BlendOperation::Add:
            return MTLBlendOperationAdd;
        case BlendOperation::Subtract:
            return MTLBlendOperationSubtract;
        case BlendOperation::ReverseSubtract:
            return MTLBlendOperationReverseSubtract;
        case BlendOperation::Min:
            return MTLBlendOperationMin;
        case BlendOperation::Max:
            return MTLBlendOperationMax;
    }

    return MTLBlendOperationAdd;
}

static MTLColorWriteMask toMetalWriteMask(const ColorWriteMask& mask)
{
    // Accumulated as the underlying integer rather than as the enum, which
    // MTLColorWriteMask is a typedef *of* rather than a bitmask type: |= on the
    // enum itself does not compile.
    NSUInteger value = MTLColorWriteMaskNone;

    if (mask.red)
        value |= MTLColorWriteMaskRed;
    if (mask.green)
        value |= MTLColorWriteMaskGreen;
    if (mask.blue)
        value |= MTLColorWriteMaskBlue;
    if (mask.alpha)
        value |= MTLColorWriteMaskAlpha;

    return (MTLColorWriteMask) value;
}

static MTLCompareFunction toMetalCompareFunction(CompareFunction compare)
{
    switch (compare)
    {
        case DepthCompare::Never:
            return MTLCompareFunctionNever;
        case DepthCompare::Less:
            return MTLCompareFunctionLess;
        case DepthCompare::LessEqual:
            return MTLCompareFunctionLessEqual;
        case DepthCompare::Equal:
            return MTLCompareFunctionEqual;
        case DepthCompare::NotEqual:
            return MTLCompareFunctionNotEqual;
        case DepthCompare::GreaterEqual:
            return MTLCompareFunctionGreaterEqual;
        case DepthCompare::Greater:
            return MTLCompareFunctionGreater;
        case DepthCompare::Always:
            return MTLCompareFunctionAlways;
    }

    return MTLCompareFunctionLessEqual;
}

static MTLStencilOperation toMetalStencilOperation(StencilOp op)
{
    switch (op)
    {
        case StencilOp::Keep:
            return MTLStencilOperationKeep;
        case StencilOp::Zero:
            return MTLStencilOperationZero;
        case StencilOp::Replace:
            return MTLStencilOperationReplace;
        case StencilOp::IncrementClamp:
            return MTLStencilOperationIncrementClamp;
        case StencilOp::DecrementClamp:
            return MTLStencilOperationDecrementClamp;
        case StencilOp::Invert:
            return MTLStencilOperationInvert;
        case StencilOp::IncrementWrap:
            return MTLStencilOperationIncrementWrap;
        case StencilOp::DecrementWrap:
            return MTLStencilOperationDecrementWrap;
    }

    return MTLStencilOperationKeep;
}

// The masks come from the descriptor rather than the face, which is where
// D3D12 keeps them - see RenderPipelineDescriptor::stencilReadMask for why the
// narrower API is the one both backends are held to.
static MTLStencilDescriptor* makeStencilDescriptor(
    const StencilFace& face, unsigned char readMask, unsigned char writeMask)
{
    auto descriptor = [[MTLStencilDescriptor alloc] init];

    descriptor.stencilCompareFunction = toMetalCompareFunction(face.compare);
    descriptor.stencilFailureOperation = toMetalStencilOperation(face.stencilFail);
    descriptor.depthFailureOperation = toMetalStencilOperation(face.depthFail);
    descriptor.depthStencilPassOperation = toMetalStencilOperation(face.pass);
    descriptor.readMask = readMask;
    descriptor.writeMask = writeMask;

    return descriptor;
}

static MTLVertexStepFunction toMetalStepFunction(StepRate rate)
{
    switch (rate)
    {
        case StepRate::PerVertex:
            return MTLVertexStepFunctionPerVertex;
        case StepRate::PerInstance:
            return MTLVertexStepFunctionPerInstance;
    }
    return MTLVertexStepFunctionPerVertex;
}

static MTLVertexDescriptor* makeVertexDescriptor(const VertexLayout& layout)
{
    if (layout.attributes.empty())
        return nil;

    auto descriptor = [MTLVertexDescriptor vertexDescriptor];

    for (auto i = 0; i < layout.attributes.size(); ++i)
    {
        const auto& attribute = layout.attributes[i];
        descriptor.attributes[i].format = toMetalVertexFormat(attribute.format);
        descriptor.attributes[i].offset = (NSUInteger) attribute.offset;
        descriptor.attributes[i].bufferIndex = (NSUInteger) attribute.bufferIndex;
    }

    // Multi-slot when `buffers` is populated; single-slot fallback otherwise
    // (pre-instancing shape). Metal needs stride and step function per bound
    // slot; a slot without an entry defaults to PerVertex with stride 0.
    if (! layout.buffers.empty())
    {
        for (auto slot = 0; slot < layout.buffers.size(); ++slot)
        {
            descriptor.layouts[slot].stride = (NSUInteger) layout.buffers[slot].stride;
            descriptor.layouts[slot].stepFunction =
                toMetalStepFunction(layout.buffers[slot].stepRate);
        }
    }
    else
    {
        descriptor.layouts[0].stride = (NSUInteger) layout.stride;
        descriptor.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
    }

    return descriptor;
}

struct RenderPipeline::Native
{
    Native(Device& device, const RenderPipelineDescriptor& descriptor)
        : topology(descriptor.topology)
        , cullMode(descriptor.cullMode)
        , frontFace(descriptor.frontFace)
    {
        auto metalDevice = (__bridge id<MTLDevice>) device.nativeDevice();

        if (metalDevice == nil || descriptor.library == nullptr)
            return;

        auto library = (__bridge id<MTLLibrary>) descriptor.library->nativeLibrary();

        if (library == nil)
            return;

        auto vertexName = @(descriptor.library->vertexEntry().c_str());
        auto fragmentName = @(descriptor.library->fragmentEntry().c_str());

        id<MTLFunction> vertexFunction = [library newFunctionWithName:vertexName];
        id<MTLFunction> fragmentFunction = [library newFunctionWithName:fragmentName];

        auto pipelineDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
        pipelineDescriptor.vertexFunction = vertexFunction;
        pipelineDescriptor.fragmentFunction = fragmentFunction;

        if (auto vertexDescriptor = makeVertexDescriptor(descriptor.vertexLayout))
            pipelineDescriptor.vertexDescriptor = vertexDescriptor;

        pipelineDescriptor.rasterSampleCount = (NSUInteger) descriptor.sampleCount;

        // One attachment carries both planes, so its format is decided by
        // whether either test is on and the stencilled one is a superset. The
        // stencil attachment names the same texture again, which is how Metal
        // spells a combined format.
        if (descriptor.depth || descriptor.stencil)
        {
            const auto attachmentFormat = descriptor.stencil
                                            ? MTLPixelFormatDepth32Float_Stencil8
                                            : MTLPixelFormatDepth32Float;

            pipelineDescriptor.depthAttachmentPixelFormat = attachmentFormat;

            if (descriptor.stencil)
                pipelineDescriptor.stencilAttachmentPixelFormat = attachmentFormat;

            auto depthDescriptor = [[MTLDepthStencilDescriptor alloc] init];

            // A pipeline that only masks the stencil still has the depth plane
            // under it, and must not disturb it: Always with no write is the
            // depth test not happening, which is what `depth` off means.
            depthDescriptor.depthCompareFunction =
                descriptor.depth ? toMetalCompareFunction(descriptor.depthCompare)
                                 : MTLCompareFunctionAlways;
            depthDescriptor.depthWriteEnabled =
                descriptor.depth && descriptor.depthWrite ? YES : NO;

            if (descriptor.stencil)
            {
                auto front = makeStencilDescriptor(descriptor.stencilFront,
                                                   descriptor.stencilReadMask,
                                                   descriptor.stencilWriteMask);
                auto back = makeStencilDescriptor(descriptor.stencilBack,
                                                  descriptor.stencilReadMask,
                                                  descriptor.stencilWriteMask);

                depthDescriptor.frontFaceStencil = front;
                depthDescriptor.backFaceStencil = back;

                [front release];
                [back release];
            }

            depthState =
                [metalDevice newDepthStencilStateWithDescriptor:depthDescriptor];
            [depthDescriptor release];
        }

        auto colorAttachment = pipelineDescriptor.colorAttachments[0];
        colorAttachment.pixelFormat = toMetalPixelFormat(descriptor.colorFormat);

        // One path for the named modes and for a written-out equation, because
        // blendStateFor turns the first into the second. A preset's meaning is
        // therefore stated once, in the header, rather than once per backend.
        const auto blend = descriptor.blend
                               ? *descriptor.blend
                               : blendStateFor(descriptor.blendMode);

        if (blend.enabled)
        {
            colorAttachment.blendingEnabled = YES;
            colorAttachment.sourceRGBBlendFactor =
                toMetalBlendFactor(blend.sourceColor);
            colorAttachment.destinationRGBBlendFactor =
                toMetalBlendFactor(blend.destinationColor);
            colorAttachment.rgbBlendOperation =
                toMetalBlendOperation(blend.colorOperation);
            colorAttachment.sourceAlphaBlendFactor =
                toMetalBlendFactor(blend.sourceAlpha);
            colorAttachment.destinationAlphaBlendFactor =
                toMetalBlendFactor(blend.destinationAlpha);
            colorAttachment.alphaBlendOperation =
                toMetalBlendOperation(blend.alphaOperation);
        }

        colorAttachment.writeMask = toMetalWriteMask(descriptor.colorWriteMask);

        NSError* error = nil;
        state = [metalDevice newRenderPipelineStateWithDescriptor:pipelineDescriptor
                                                            error:&error];

        if (state.get() == nil && error != nil)
            LOG(error.localizedDescription.UTF8String);

        [vertexFunction release];
        [fragmentFunction release];
        [pipelineDescriptor release];
    }

    PrimitiveTopology topology = PrimitiveTopology::Triangles;
    CullMode cullMode = CullMode::None;
    Winding frontFace = Winding::CounterClockwise;
    ObjC::Ptr<NSObject<MTLRenderPipelineState>> state;
    ObjC::Ptr<NSObject<MTLDepthStencilState>> depthState;
};

RenderPipeline::RenderPipeline(Device& device,
                               const RenderPipelineDescriptor& descriptor)
    : impl(device, descriptor)
{
}

bool RenderPipeline::isValid() const
{
    return impl->state.get() != nil;
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
    return (__bridge void*) impl->state.get();
}

void* RenderPipeline::nativeDepthState() const
{
    return (__bridge void*) impl->depthState.get();
}
} // namespace eacp::GPU
