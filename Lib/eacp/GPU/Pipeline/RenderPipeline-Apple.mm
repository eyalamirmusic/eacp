#import <Metal/Metal.h>

#include "RenderPipeline.h"

#include "../Device/Device.h"
#include "../Device/MetalErrors-Apple.h"
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

        // The integer formats are the plain ones and the Norm ones the
        // Normalized ones, as above; D3D12's UINT/SINT against UNORM/SNORM and
        // Vulkan's the same. See VertexLayout.h for which a shader reads how.
        case VertexFormat::UByte:
            return MTLVertexFormatUChar;
        case VertexFormat::UByte2:
            return MTLVertexFormatUChar2;
        case VertexFormat::UByte4:
            return MTLVertexFormatUChar4;
        case VertexFormat::UByteNorm:
            return MTLVertexFormatUCharNormalized;
        case VertexFormat::UByte2Norm:
            return MTLVertexFormatUChar2Normalized;
        case VertexFormat::Byte:
            return MTLVertexFormatChar;
        case VertexFormat::Byte2:
            return MTLVertexFormatChar2;
        case VertexFormat::Byte4:
            return MTLVertexFormatChar4;
        case VertexFormat::ByteNorm:
            return MTLVertexFormatCharNormalized;
        case VertexFormat::Byte2Norm:
            return MTLVertexFormatChar2Normalized;
        case VertexFormat::Byte4Norm:
            return MTLVertexFormatChar4Normalized;
        case VertexFormat::UShort:
            return MTLVertexFormatUShort;
        case VertexFormat::UShort2:
            return MTLVertexFormatUShort2;
        case VertexFormat::UShort4:
            return MTLVertexFormatUShort4;
        case VertexFormat::UShortNorm:
            return MTLVertexFormatUShortNormalized;
        case VertexFormat::UShort2Norm:
            return MTLVertexFormatUShort2Normalized;
        case VertexFormat::UShort4Norm:
            return MTLVertexFormatUShort4Normalized;
        case VertexFormat::Short:
            return MTLVertexFormatShort;
        case VertexFormat::Short2:
            return MTLVertexFormatShort2;
        case VertexFormat::Short4:
            return MTLVertexFormatShort4;
        case VertexFormat::ShortNorm:
            return MTLVertexFormatShortNormalized;
        case VertexFormat::Half:
            return MTLVertexFormatHalf;
        case VertexFormat::UInt1010102Norm:
            return MTLVertexFormatUInt1010102Normalized;
        case VertexFormat::UInt:
            return MTLVertexFormatUInt;
        case VertexFormat::UInt2:
            return MTLVertexFormatUInt2;
        case VertexFormat::UInt3:
            return MTLVertexFormatUInt3;
        case VertexFormat::UInt4:
            return MTLVertexFormatUInt4;
        case VertexFormat::Int:
            return MTLVertexFormatInt;
        case VertexFormat::Int2:
            return MTLVertexFormatInt2;
        case VertexFormat::Int3:
            return MTLVertexFormatInt3;
        case VertexFormat::Int4:
            return MTLVertexFormatInt4;
    }

    return MTLVertexFormatFloat3;
}

// Defined in Texture-Apple.mm: one table of Metal formats, which every
// attachment format reaches through the texture format it is the attachment of.
MTLPixelFormat metalTextureFormat(TextureFormat format);

static MTLPixelFormat toMetalPixelFormat(PixelFormat format)
{
    return metalTextureFormat(textureFormatFor(format));
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

// Named on every pipeline because a vertex shader writing
// [[render_target_array_index]] needs it on macOS, and naming the class the
// draws already use costs nothing anywhere else.
static MTLPrimitiveTopologyClass toMetalTopologyClass(PrimitiveTopology topology)
{
    switch (topology)
    {
        case PrimitiveTopology::Triangles:
        case PrimitiveTopology::TriangleStrip:
            return MTLPrimitiveTopologyClassTriangle;
        case PrimitiveTopology::Lines:
        case PrimitiveTopology::LineStrip:
            return MTLPrimitiveTopologyClassLine;
        case PrimitiveTopology::Points:
            return MTLPrimitiveTopologyClassPoint;
    }

    return MTLPrimitiveTopologyClassTriangle;
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

// One path for the named modes and for a written-out equation, because
// resolvedBlend turns the first into the second. A preset's meaning is
// therefore stated once, in the header, rather than once per backend.
static void describeColorTarget(MTLRenderPipelineColorAttachmentDescriptor* attachment,
                                const ColorTargetState& target)
{
    attachment.pixelFormat = toMetalPixelFormat(target.format);

    const auto blend = resolvedBlend(target);

    if (blend.enabled)
    {
        attachment.blendingEnabled = YES;
        attachment.sourceRGBBlendFactor = toMetalBlendFactor(blend.sourceColor);
        attachment.destinationRGBBlendFactor =
            toMetalBlendFactor(blend.destinationColor);
        attachment.rgbBlendOperation = toMetalBlendOperation(blend.colorOperation);
        attachment.sourceAlphaBlendFactor = toMetalBlendFactor(blend.sourceAlpha);
        attachment.destinationAlphaBlendFactor =
            toMetalBlendFactor(blend.destinationAlpha);
        attachment.alphaBlendOperation = toMetalBlendOperation(blend.alphaOperation);
    }

    attachment.writeMask = toMetalWriteMask(target.writeMask);
}

struct RenderPipeline::Native
{
    Native(Device& device, const RenderPipelineDescriptor& descriptor)
        : topology(descriptor.topology)
        , cullMode(descriptor.cullMode)
        , frontFace(descriptor.frontFace)
        , depthBias(appliedDepthBias(descriptor))
    {
        auto metalDevice = (__bridge id<MTLDevice>) device.nativeDevice();

        if (metalDevice == nil || descriptor.library == nullptr)
            return;

        auto library = (__bridge id<MTLLibrary>) descriptor.library->nativeLibrary();

        if (library == nil)
            return;

        const auto hasFragment = descriptor.library->hasFragment();
        auto vertexName = @(descriptor.library->vertexEntry().c_str());
        auto fragmentName = @(descriptor.library->fragmentEntry().c_str());

        id<MTLFunction> vertexFunction = [library newFunctionWithName:vertexName];
        id<MTLFunction> fragmentFunction =
            hasFragment ? [library newFunctionWithName:fragmentName] : nil;

        if (hasFragment && fragmentFunction == nil)
        {
            LOG("eacp: no fragment function named ",
                descriptor.library->fragmentEntry(),
                " in the library");
            [vertexFunction release];
            return;
        }

        auto pipelineDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
        pipelineDescriptor.vertexFunction = vertexFunction;
        pipelineDescriptor.fragmentFunction = fragmentFunction;

        if (auto vertexDescriptor = makeVertexDescriptor(descriptor.vertexLayout))
            pipelineDescriptor.vertexDescriptor = vertexDescriptor;

        pipelineDescriptor.rasterSampleCount = (NSUInteger) descriptor.sampleCount;
        pipelineDescriptor.inputPrimitiveTopology =
            toMetalTopologyClass(descriptor.topology);

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

        if (descriptor.colorTargetCount() > maxColorAttachments)
        {
            LOG("eacp: a pipeline writing ",
                descriptor.colorTargetCount(),
                " colour targets is refused; the most is ",
                maxColorAttachments);
            [vertexFunction release];
            [fragmentFunction release];
            [pipelineDescriptor release];
            return;
        }

        for (auto index = 0; index < descriptor.colorTargetCount(); ++index)
            describeColorTarget(pipelineDescriptor.colorAttachments[(NSUInteger) index],
                                writtenColorTarget(descriptor, index, hasFragment));

        auto build = [metalDevice, pipelineDescriptor](NSError** error)
        {
            return [metalDevice newRenderPipelineStateWithDescriptor:pipelineDescriptor
                                                               error:error];
        };

        state = Detail::buildRetryingTransientFailures<id<MTLRenderPipelineState>>(
            "building a Metal render pipeline", build);

        [vertexFunction release];
        [fragmentFunction release];
        [pipelineDescriptor release];
    }

    PrimitiveTopology topology = PrimitiveTopology::Triangles;
    CullMode cullMode = CullMode::None;
    Winding frontFace = Winding::CounterClockwise;
    DepthBias depthBias;
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

DepthBias RenderPipeline::depthBias() const
{
    return impl->depthBias;
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
