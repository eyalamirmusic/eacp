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
    }

    return MTLPixelFormatBGRA8Unorm;
}

static MTLCompareFunction toMetalCompareFunction(DepthCompare compare)
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

        if (descriptor.depth)
        {
            pipelineDescriptor.depthAttachmentPixelFormat =
                MTLPixelFormatDepth32Float;

            auto depthDescriptor = [[MTLDepthStencilDescriptor alloc] init];
            depthDescriptor.depthCompareFunction =
                toMetalCompareFunction(descriptor.depthCompare);
            depthDescriptor.depthWriteEnabled = descriptor.depthWrite ? YES : NO;
            depthState =
                [metalDevice newDepthStencilStateWithDescriptor:depthDescriptor];
            [depthDescriptor release];
        }

        auto colorAttachment = pipelineDescriptor.colorAttachments[0];
        colorAttachment.pixelFormat = toMetalPixelFormat(descriptor.colorFormat);

        switch (descriptor.blendMode)
        {
            case BlendMode::None:
                break;
            case BlendMode::AlphaBlend:
                colorAttachment.blendingEnabled = YES;
                colorAttachment.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
                colorAttachment.destinationRGBBlendFactor =
                    MTLBlendFactorOneMinusSourceAlpha;
                colorAttachment.sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
                colorAttachment.destinationAlphaBlendFactor =
                    MTLBlendFactorOneMinusSourceAlpha;
                break;
            case BlendMode::AlphaBlendOntoTransparent:
                colorAttachment.blendingEnabled = YES;
                colorAttachment.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
                colorAttachment.destinationRGBBlendFactor =
                    MTLBlendFactorOneMinusSourceAlpha;
                colorAttachment.sourceAlphaBlendFactor = MTLBlendFactorOne;
                colorAttachment.destinationAlphaBlendFactor =
                    MTLBlendFactorOneMinusSourceAlpha;
                break;
            case BlendMode::Additive:
                colorAttachment.blendingEnabled = YES;
                colorAttachment.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
                colorAttachment.destinationRGBBlendFactor = MTLBlendFactorOne;
                colorAttachment.sourceAlphaBlendFactor = MTLBlendFactorOne;
                colorAttachment.destinationAlphaBlendFactor = MTLBlendFactorOne;
                break;
            default:
                // Guards against a future BlendMode value that this backend
                // was never taught to handle - would otherwise silently
                // produce a no-blend pipeline. Loud in Debug, degrades to
                // None in Release (both backends match this behaviour).
                assert(false && "eacp: unhandled BlendMode in Metal backend");
                break;
        }

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
