#import <Metal/Metal.h>

#include "RenderPass.h"

#include "../Buffer/Buffer.h"
#include "../Pipeline/RenderPipeline.h"
#include "../Texture/Texture.h"

#include <eacp/Core/ObjC/ObjC.h>

#include <algorithm>
#include <cmath>

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
    Native(void* encoderHandle, int width, int height)
        : targetWidth(width)
        , targetHeight(height)
    {
        if (encoderHandle != nullptr)
            encoder.reset((__bridge NSObject<MTLRenderCommandEncoder>*) encoderHandle);
    }

    ObjC::Ptr<NSObject<MTLRenderCommandEncoder>> encoder;

    // Render target size in pixels, for clamping scissor rects.
    int targetWidth = 0;
    int targetHeight = 0;

    // Metal takes the primitive type per draw call, so the pass remembers the
    // bound pipeline's topology.
    MTLPrimitiveType primitiveType = MTLPrimitiveTypeTriangle;
    bool ended = false;

    // Whether a valid pipeline state is currently bound. A pipeline whose
    // compilation failed has a nil state; drawing without one aborts under Metal
    // API validation (the Xcode debug default), so draws are skipped when false.
    bool pipelineBound = false;
};

RenderPass::RenderPass(void* encoder, int targetWidth, int targetHeight)
    : impl(encoder, targetWidth, targetHeight)
{
}

RenderPass::~RenderPass()
{
    end();
}

void RenderPass::setScissorRect(const Graphics::Rect& rect)
{
    auto activeEncoder = impl->encoder.get();

    if (activeEncoder == nil || impl->targetWidth <= 0 || impl->targetHeight <= 0)
        return;

    // Round outward before clamping: rounding a scrolled region's edge inward
    // would shave a column of glyph coverage off the boundary.
    const auto left = std::clamp((int) std::floor(rect.x), 0, impl->targetWidth);
    const auto top = std::clamp((int) std::floor(rect.y), 0, impl->targetHeight);
    const auto right =
        std::clamp((int) std::ceil(rect.x + rect.w), left, impl->targetWidth);
    const auto bottom =
        std::clamp((int) std::ceil(rect.y + rect.h), top, impl->targetHeight);

    const MTLScissorRect scissor {(NSUInteger) left,
                                  (NSUInteger) top,
                                  (NSUInteger) (right - left),
                                  (NSUInteger) (bottom - top)};

    [activeEncoder setScissorRect:scissor];
}

void RenderPass::clearScissorRect()
{
    auto activeEncoder = impl->encoder.get();

    if (activeEncoder == nil || impl->targetWidth <= 0 || impl->targetHeight <= 0)
        return;

    const MTLScissorRect scissor {
        0, 0, (NSUInteger) impl->targetWidth, (NSUInteger) impl->targetHeight};

    [activeEncoder setScissorRect:scissor];
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
    // Uniforms live at buffer(uniformBase + slot) so multi-slot vertex
    // layouts (e.g. instancing with slots 0..N) never collide with the
    // uniform bind. Matches ComputePass::uniformBase.
    if (auto activeEncoder = impl->encoder.get())
        [activeEncoder setVertexBytes:data
                               length:bytes
                              atIndex:(NSUInteger) (uniformBase + slot)];
}

void RenderPass::setFragmentBytes(const void* data, std::size_t bytes, int slot)
{
    // Same uniformBase mapping as the vertex stage, so one slot rule covers
    // both; the generated fragment functions declare the block at
    // buffer(uniformBase).
    if (auto activeEncoder = impl->encoder.get())
        [activeEncoder setFragmentBytes:data
                                 length:bytes
                                atIndex:(NSUInteger) (uniformBase + slot)];
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

void RenderPass::drawInstanced(int vertexCount,
                               int instanceCount,
                               int firstVertex,
                               int firstInstance)
{
    if (! impl->pipelineBound)
        return;

    if (auto activeEncoder = impl->encoder.get())
        [activeEncoder drawPrimitives:impl->primitiveType
                          vertexStart:(NSUInteger) firstVertex
                          vertexCount:(NSUInteger) vertexCount
                        instanceCount:(NSUInteger) instanceCount
                         baseInstance:(NSUInteger) firstInstance];
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

void RenderPass::drawIndexedInstanced(const Buffer& indices,
                                      int indexCount,
                                      int instanceCount,
                                      IndexFormat format,
                                      int firstIndex,
                                      int firstInstance)
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
                       indexBufferOffset:(NSUInteger) firstIndex * indexSize
                           instanceCount:(NSUInteger) instanceCount
                              baseVertex:0
                            baseInstance:(NSUInteger) firstInstance];
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
