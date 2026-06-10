#import <Metal/Metal.h>

#include "RenderPass.h"

#include "../Buffer/Buffer.h"
#include "../Pipeline/RenderPipeline.h"
#include "../Texture/Texture.h"

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

    if (auto depthState =
            (__bridge id<MTLDepthStencilState>) pipeline.nativeDepthState())
        [activeEncoder setDepthStencilState:depthState];
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

void RenderPass::setFragmentTexture(const Texture& texture, int slot)
{
    auto activeEncoder = impl->encoder.get();
    auto metalTexture = (__bridge id<MTLTexture>) texture.nativeTexture();
    auto metalSampler = (__bridge id<MTLSamplerState>) texture.nativeSampler();

    if (activeEncoder == nil || metalTexture == nil || metalSampler == nil)
        return;

    [activeEncoder setFragmentTexture:metalTexture atIndex:(NSUInteger) slot];
    [activeEncoder setFragmentSamplerState:metalSampler atIndex:(NSUInteger) slot];
}

void RenderPass::setVertexBytes(const void* data, std::size_t bytes, int slot)
{
    // Vertex data is bound at buffer index 0, so uniform slot 0 maps to buffer 1.
    if (auto activeEncoder = impl->encoder.get())
        [activeEncoder setVertexBytes:data
                               length:bytes
                              atIndex:(NSUInteger) (1 + slot)];
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
