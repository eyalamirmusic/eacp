#import <Metal/Metal.h>

#include "RenderPass.h"

#include "../Buffer/Buffer.h"
#include "../Device/Device.h"
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

MTLCullMode toMetalCullMode(CullMode mode)
{
    switch (mode)
    {
        case CullMode::None:
            return MTLCullModeNone;
        case CullMode::Front:
            return MTLCullModeFront;
        case CullMode::Back:
            return MTLCullModeBack;
    }

    return MTLCullModeNone;
}

MTLWinding toMetalWinding(Winding winding)
{
    switch (winding)
    {
        case Winding::CounterClockwise:
            return MTLWindingCounterClockwise;
        case Winding::Clockwise:
            return MTLWindingClockwise;
    }

    return MTLWindingCounterClockwise;
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

    // A failed pipeline has a nil state, and drawing without one aborts under
    // Metal API validation, so draws are skipped when false.
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

void RenderPass::setViewport(const Graphics::Rect& rect,
                             float nearDepth,
                             float farDepth)
{
    auto activeEncoder = impl->encoder.get();

    if (activeEncoder == nil || impl->targetWidth <= 0 || impl->targetHeight <= 0)
        return;

    // No rounding, unlike the scissor: a viewport is a float rectangle in both
    // APIs, and rounding it would move the mapping rather than the clip.
    if (rect.w <= 0.f || rect.h <= 0.f || rect.x < 0.f || rect.y < 0.f
        || rect.x + rect.w > (float) impl->targetWidth
        || rect.y + rect.h > (float) impl->targetHeight)
        return;

    const MTLViewport viewport {rect.x, rect.y, rect.w, rect.h, nearDepth, farDepth};

    [activeEncoder setViewport:viewport];
}

void RenderPass::clearViewport()
{
    auto activeEncoder = impl->encoder.get();

    if (activeEncoder == nil || impl->targetWidth <= 0 || impl->targetHeight <= 0)
        return;

    const MTLViewport viewport {
        0.0, 0.0, (double) impl->targetWidth, (double) impl->targetHeight, 0.0, 1.0};

    [activeEncoder setViewport:viewport];
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

    // Culling is encoder state on Metal, so set on every setPipeline or a culled
    // pipeline leaves its mode behind. Facing is decided in clip space, before
    // the viewport's y flip - see CullMode and Tests/GPU/CullModeTests.cpp.
    if (impl->pipelineBound)
    {
        [activeEncoder setFrontFacingWinding:toMetalWinding(pipeline.frontFace())];
        [activeEncoder setCullMode:toMetalCullMode(pipeline.cullMode())];
    }

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

void RenderPass::setFragmentTexture(const Texture& texture,
                                    int slot,
                                    TextureSampling sampling)
{
    auto activeEncoder = impl->encoder.get();
    auto metalTexture = (__bridge id<MTLTexture>) texture.nativeTexture();

    // Keyed on the sampling the shader declared, not one the texture carries,
    // to agree with D3D12 where the declaration is all there is.
    auto metalSampler =
        (__bridge id<MTLSamplerState>) Device::shared().nativeSampler(sampling);

    if (activeEncoder == nil || metalTexture == nil || metalSampler == nil)
        return;

    [activeEncoder setFragmentTexture:metalTexture atIndex:(NSUInteger) slot];
    [activeEncoder setFragmentSamplerState:metalSampler atIndex:(NSUInteger) slot];
}

void RenderPass::setVertexStorageBuffer(const Buffer& buffer, int slot)
{
    auto activeEncoder = impl->encoder.get();
    auto metalBuffer = (__bridge id<MTLBuffer>) buffer.nativeBuffer();

    if (activeEncoder != nil && metalBuffer != nil)
        [activeEncoder setVertexBuffer:metalBuffer
                                offset:0
                               atIndex:(NSUInteger) (bufferBase + slot)];
}

void RenderPass::setFragmentStorageBuffer(const Buffer& buffer, int slot)
{
    auto activeEncoder = impl->encoder.get();
    auto metalBuffer = (__bridge id<MTLBuffer>) buffer.nativeBuffer();

    if (activeEncoder != nil && metalBuffer != nil)
        [activeEncoder setFragmentBuffer:metalBuffer
                                  offset:0
                                 atIndex:(NSUInteger) (bufferBase + slot)];
}

void RenderPass::setVertexBytes(const void* data, std::size_t bytes, int slot)
{
    // Offset by uniformBase so multi-slot vertex layouts never collide.
    if (auto activeEncoder = impl->encoder.get())
        [activeEncoder setVertexBytes:data
                               length:bytes
                              atIndex:(NSUInteger) (uniformBase + slot)];
}

void RenderPass::setFragmentBytes(const void* data, std::size_t bytes, int slot)
{
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
                             int firstIndex,
                             int baseVertex)
{
    auto activeEncoder = impl->encoder.get();
    auto metalBuffer = (__bridge id<MTLBuffer>) indices.nativeBuffer();

    if (! impl->pipelineBound || activeEncoder == nil || metalBuffer == nil)
        return;

    auto indexType = format == IndexFormat::UInt16 ? MTLIndexTypeUInt16
                                                   : MTLIndexTypeUInt32;
    auto indexSize = format == IndexFormat::UInt16 ? sizeof(std::uint16_t)
                                                   : sizeof(std::uint32_t);

    // The long selector because only it carries a base vertex.
    [activeEncoder drawIndexedPrimitives:impl->primitiveType
                              indexCount:(NSUInteger) indexCount
                               indexType:indexType
                             indexBuffer:metalBuffer
                       indexBufferOffset:(NSUInteger) firstIndex * indexSize
                           instanceCount:1
                              baseVertex:(NSInteger) baseVertex
                            baseInstance:0];
}

void RenderPass::drawIndexedInstanced(const Buffer& indices,
                                      int indexCount,
                                      int instanceCount,
                                      IndexFormat format,
                                      int firstIndex,
                                      int firstInstance,
                                      int baseVertex)
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
                              baseVertex:(NSInteger) baseVertex
                            baseInstance:(NSUInteger) firstInstance];
}

void RenderPass::end()
{
    if (impl->ended)
        return;

    // Before endEncoding, so queued draws still reach this encoder.
    drainParticipants();

    if (auto activeEncoder = impl->encoder.get())
        [activeEncoder endEncoding];

    impl->ended = true;
}
} // namespace eacp::GPU
