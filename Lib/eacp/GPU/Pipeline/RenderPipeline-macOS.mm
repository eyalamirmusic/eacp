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
    }

    return MTLVertexFormatFloat3;
}

static MTLPixelFormat toMetalPixelFormat(PixelFormat format)
{
    switch (format)
    {
        case PixelFormat::BGRA8Unorm:
            return MTLPixelFormatBGRA8Unorm;
    }

    return MTLPixelFormatBGRA8Unorm;
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

    descriptor.layouts[0].stride = (NSUInteger) layout.stride;
    return descriptor;
}

struct RenderPipeline::Native
{
    Native(Device& device, const RenderPipelineDescriptor& descriptor)
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
            depthDescriptor.depthCompareFunction = MTLCompareFunctionLessEqual;
            depthDescriptor.depthWriteEnabled = YES;
            depthState =
                [metalDevice newDepthStencilStateWithDescriptor:depthDescriptor];
            [depthDescriptor release];
        }

        auto colorAttachment = pipelineDescriptor.colorAttachments[0];
        colorAttachment.pixelFormat = toMetalPixelFormat(descriptor.colorFormat);

        if (descriptor.blending)
        {
            colorAttachment.blendingEnabled = YES;
            colorAttachment.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
            colorAttachment.destinationRGBBlendFactor =
                MTLBlendFactorOneMinusSourceAlpha;
            colorAttachment.sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
            colorAttachment.destinationAlphaBlendFactor =
                MTLBlendFactorOneMinusSourceAlpha;
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

void* RenderPipeline::nativeState() const
{
    return (__bridge void*) impl->state.get();
}

void* RenderPipeline::nativeDepthState() const
{
    return (__bridge void*) impl->depthState.get();
}
} // namespace eacp::GPU
