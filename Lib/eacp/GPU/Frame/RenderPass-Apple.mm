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

int RenderPass::targetWidth() const
{
    return impl->targetWidth;
}

int RenderPass::targetHeight() const
{
    return impl->targetHeight;
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

    // Face culling is encoder state on Metal and pipeline state on D3D12, so it
    // travels on the pipeline and is applied here - which also means both of
    // these have to be set on every setPipeline rather than only on the culling
    // ones: encoder state persists, so a culled pipeline would otherwise leave
    // its mode behind for whatever draws next.
    //
    // The default winding is CounterClockwise, which is not Metal's own and is
    // not a preference either: it is what makes this backend mean by "front"
    // what CullMode says eacp means - counter-clockwise in *clip* space. Metal
    // decides facing there, before the viewport's y flip, which is measured
    // rather than assumed (Tests/GPU/CullModeTests.cpp) and is the opposite end
    // of that flip from D3D12's screen-space rule. Leaving both backends on
    // their own defaults would have culled opposite faces from the same mesh.
    if (impl->pipelineBound)
    {
        [activeEncoder setFrontFacingWinding:toMetalWinding(pipeline.frontFace())];
        [activeEncoder setCullMode:toMetalCullMode(pipeline.cullMode())];
    }

    impl->primitiveType = toMetalPrimitiveType(pipeline.topology());
}

void RenderPass::setStencilReference(unsigned int value)
{
    if (auto activeEncoder = impl->encoder.get())
        [activeEncoder setStencilReferenceValue:(uint32_t) value];
}

void RenderPass::setVertexBuffer(const Buffer& buffer, int index)
{
    setVertexBuffer(BufferRange::of(buffer), index);
}

void RenderPass::setVertexBuffer(const BufferRange& range, int index)
{
    auto activeEncoder = impl->encoder.get();
    auto metalBuffer = range.buffer != nullptr
                           ? (__bridge id<MTLBuffer>) range.buffer->nativeBuffer()
                           : nil;

    // The offset is the range's whole contribution: Metal reads vertex zero
    // at buffer + offset, which is exactly what a slice of an arena is.
    if (activeEncoder != nil && metalBuffer != nil)
        [activeEncoder setVertexBuffer:metalBuffer
                                offset:(NSUInteger) range.offset
                               atIndex:(NSUInteger) index];
}

void RenderPass::setFragmentTexture(const Texture& texture,
                                    int slot,
                                    TextureSampling sampling)
{
    auto activeEncoder = impl->encoder.get();
    auto metalTexture = (__bridge id<MTLTexture>) texture.nativeTexture();

    // The state for the sampling the shader declared, not one the texture
    // carries — that is what keeps this backend agreeing with D3D12, where the
    // declaration is the only thing that can pick a sampler at all.
    auto metalSampler =
        (__bridge id<MTLSamplerState>) Device::shared().nativeSampler(sampling);

    if (activeEncoder == nil || metalTexture == nil || metalSampler == nil)
        return;

    [activeEncoder setFragmentTexture:metalTexture atIndex:(NSUInteger) slot];
    [activeEncoder setFragmentSamplerState:metalSampler atIndex:(NSUInteger) slot];
}

void RenderPass::setFragmentDepthTexture(const Texture& renderTarget,
                                         int slot,
                                         TextureSampling sampling)
{
    if (!renderTarget.hasSampleableDepth())
        return;

    auto activeEncoder = impl->encoder.get();
    auto metalTexture = (__bridge id<MTLTexture>) renderTarget.nativeDepthTexture();

    auto metalSampler =
        (__bridge id<MTLSamplerState>) Device::shared().nativeSampler(sampling);

    if (activeEncoder == nil || metalTexture == nil || metalSampler == nil)
        return;

    // The same two calls the colour bind makes, which is the whole of the
    // difference on this backend: a depth texture goes on a texture index like
    // any other, and what makes it a depth2d rather than a texture2d is the
    // declaration the shader was compiled with.
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
                             int firstIndex,
                             int baseVertex)
{
    drawIndexed(BufferRange::of(indices), indexCount, format, firstIndex, baseVertex);
}

void RenderPass::drawIndexed(const BufferRange& indices,
                             int indexCount,
                             IndexFormat format,
                             int firstIndex,
                             int baseVertex)
{
    auto activeEncoder = impl->encoder.get();
    auto metalBuffer = indices.buffer != nullptr
                           ? (__bridge id<MTLBuffer>) indices.buffer->nativeBuffer()
                           : nil;

    if (! impl->pipelineBound || activeEncoder == nil || metalBuffer == nil)
        return;

    auto indexType = format == IndexFormat::UInt16 ? MTLIndexTypeUInt16
                                                   : MTLIndexTypeUInt32;
    auto indexSize = format == IndexFormat::UInt16 ? sizeof(std::uint16_t)
                                                   : sizeof(std::uint32_t);

    // The eight-argument selector rather than the five-argument one because
    // only this form carries a base vertex; instanceCount:1 makes it the same
    // draw the short form issues. The range's offset and firstIndex are the
    // same thing to Metal - a byte offset into the buffer - so they add.
    [activeEncoder drawIndexedPrimitives:impl->primitiveType
                              indexCount:(NSUInteger) indexCount
                               indexType:indexType
                             indexBuffer:metalBuffer
                       indexBufferOffset:(NSUInteger) (indices.offset
                                                       + (std::size_t) firstIndex
                                                             * indexSize)
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
    drawIndexedInstanced(BufferRange::of(indices),
                         indexCount,
                         instanceCount,
                         format,
                         firstIndex,
                         firstInstance,
                         baseVertex);
}

void RenderPass::drawIndexedInstanced(const BufferRange& indices,
                                      int indexCount,
                                      int instanceCount,
                                      IndexFormat format,
                                      int firstIndex,
                                      int firstInstance,
                                      int baseVertex)
{
    auto activeEncoder = impl->encoder.get();
    auto metalBuffer = indices.buffer != nullptr
                           ? (__bridge id<MTLBuffer>) indices.buffer->nativeBuffer()
                           : nil;

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
                       indexBufferOffset:(NSUInteger) (indices.offset
                                                       + (std::size_t) firstIndex
                                                             * indexSize)
                           instanceCount:(NSUInteger) instanceCount
                              baseVertex:(NSInteger) baseVertex
                            baseInstance:(NSUInteger) firstInstance];
}

void RenderPass::end()
{
    if (impl->ended)
        return;

    // Before endEncoding, so a batching renderer's queued draws still reach
    // this encoder. See RenderPass::Participant.
    drainParticipants();

    if (auto activeEncoder = impl->encoder.get())
        [activeEncoder endEncoding];

    impl->ended = true;
}
} // namespace eacp::GPU
