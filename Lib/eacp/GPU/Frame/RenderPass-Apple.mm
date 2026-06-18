#import <Metal/Metal.h>

#include "RenderPass.h"

#include "../Buffer/Buffer.h"
#include "../Pipeline/RenderPipeline.h"
#include "../Texture/Texture.h"

#include <eacp/Core/ObjC/ObjC.h>

#include <cstdint>

namespace eacp::GPU
{
namespace
{
MTLPrimitiveType toMetalPrimitiveType(PrimitiveTopology topology)
{
    switch (topology)
    {
        case PrimitiveTopology::Triangles:
            return MTLPrimitiveTypeTriangle;
        case PrimitiveTopology::TriangleStrip:
            return MTLPrimitiveTypeTriangleStrip;
        case PrimitiveTopology::Lines:
            return MTLPrimitiveTypeLine;
        case PrimitiveTopology::LineStrip:
            return MTLPrimitiveTypeLineStrip;
        case PrimitiveTopology::Points:
            return MTLPrimitiveTypePoint;
    }

    return MTLPrimitiveTypeTriangle;
}
} // namespace

struct RenderPass::Native
{
    explicit Native(void* encoderHandle)
    {
        if (encoderHandle != nullptr)
            encoder.reset((__bridge NSObject<MTLRenderCommandEncoder>*) encoderHandle);
    }

    ObjC::Ptr<NSObject<MTLRenderCommandEncoder>> encoder;

    // Metal takes the primitive type per draw call, so the pass remembers the
    // bound pipeline's topology.
    MTLPrimitiveType primitiveType = MTLPrimitiveTypeTriangle;
    bool ended = false;

    // Whether a valid pipeline state is currently bound. A pipeline whose
    // compilation failed has a nil state; drawing without one aborts under Metal
    // API validation (the Xcode debug default), so draws are skipped when false.
    bool pipelineBound = false;
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

    impl->pipelineBound = activeEncoder != nil && state != nil;

    if (impl->pipelineBound)
        [activeEncoder setRenderPipelineState:state];

    if (auto depthState =
            (__bridge id<MTLDepthStencilState>) pipeline.nativeDepthState())
        [activeEncoder setDepthStencilState:depthState];

    impl->primitiveType = toMetalPrimitiveType(pipeline.topology());
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

void RenderPass::setFragmentBytes(const void* data, std::size_t bytes, int slot)
{
    // Same 1 + slot mapping as the vertex stage, so one slot rule covers both;
    // the generated fragment functions declare the block at buffer(1).
    if (auto activeEncoder = impl->encoder.get())
        [activeEncoder setFragmentBytes:data
                                 length:bytes
                                atIndex:(NSUInteger) (1 + slot)];
}

void RenderPass::draw(int vertexCount, int firstVertex)
{
    if (! impl->pipelineBound)
        return;

    if (auto activeEncoder = impl->encoder.get())
        [activeEncoder drawPrimitives:impl->primitiveType
                          vertexStart:(NSUInteger) firstVertex
                          vertexCount:(NSUInteger) vertexCount];
}

void RenderPass::drawIndexed(const Buffer& indices,
                             int indexCount,
                             IndexFormat format,
                             int firstIndex)
{
    auto activeEncoder = impl->encoder.get();
    auto metalBuffer = (__bridge id<MTLBuffer>) indices.nativeBuffer();

    if (! impl->pipelineBound || activeEncoder == nil || metalBuffer == nil)
        return;

    auto indexType = format == IndexFormat::UInt16 ? MTLIndexTypeUInt16
                                                   : MTLIndexTypeUInt32;
    auto indexSize = format == IndexFormat::UInt16 ? sizeof(std::uint16_t)
                                                   : sizeof(std::uint32_t);

    [activeEncoder drawIndexedPrimitives:impl->primitiveType
                              indexCount:(NSUInteger) indexCount
                               indexType:indexType
                             indexBuffer:metalBuffer
                       indexBufferOffset:(NSUInteger) firstIndex * indexSize];
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
