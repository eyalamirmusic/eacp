#import <Metal/Metal.h>

#include "RenderPass.h"

#include "../Buffer/Buffer.h"
#include "../Pipeline/RenderPipeline.h"

#include <eacp/Core/ObjC/ObjC.h>

namespace eacp::GPU
{
struct RenderPass::Native
{
    explicit Native(void* encoderHandle)
    {
        if (encoderHandle != nullptr)
            encoder.reset((__bridge NSObject<MTLRenderCommandEncoder>*) encoderHandle);
    }

    ObjC::Ptr<NSObject<MTLRenderCommandEncoder>> encoder;
    bool ended = false;
};

RenderPass::RenderPass(void* encoder)
    : impl(encoder)
{
}

RenderPass::~RenderPass()
{
    end();
}

void RenderPass::setPipeline(const RenderPipeline& pipeline)
{
    auto activeEncoder = impl->encoder.get();
    auto state = (__bridge id<MTLRenderPipelineState>) pipeline.nativeState();

    if (activeEncoder != nil && state != nil)
        [activeEncoder setRenderPipelineState:state];
}

void RenderPass::setVertexBuffer(const Buffer& buffer, int index)
{
    auto activeEncoder = impl->encoder.get();
    auto metalBuffer = (__bridge id<MTLBuffer>) buffer.nativeBuffer();

    if (activeEncoder != nil && metalBuffer != nil)
        [activeEncoder setVertexBuffer:metalBuffer
                                offset:0
                               atIndex:(NSUInteger) index];
}

void RenderPass::draw(int vertexCount, int firstVertex)
{
    if (auto activeEncoder = impl->encoder.get())
        [activeEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                          vertexStart:(NSUInteger) firstVertex
                          vertexCount:(NSUInteger) vertexCount];
}

void RenderPass::end()
{
    if (impl->ended)
        return;

    if (auto activeEncoder = impl->encoder.get())
        [activeEncoder endEncoding];

    impl->ended = true;
}
} // namespace eacp::GPU
