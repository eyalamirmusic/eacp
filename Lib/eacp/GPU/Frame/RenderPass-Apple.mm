#import <Metal/Metal.h>

#include "RenderPass.h"

#include "IndirectDrawPlan.h"

#include "../Buffer/Buffer.h"
#include "../Device/Device.h"
#include "../Pipeline/RenderPipeline.h"
#include "../Shader/ShaderSource.h"
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

bool isBindableRange(const BufferRange& range)
{
    return range.isValid() && range.offset >= 0
           && range.offset < range.buffer->size();
}

id<MTLBuffer> metalBufferOf(const BufferRange& range)
{
    if (!isBindableRange(range))
        return nil;

    return (__bridge id<MTLBuffer>) range.buffer->nativeBuffer();
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

RenderPass::RenderPass(void* encoder,
                       int targetWidth,
                       int targetHeight,
                       OcclusionQueries* queries)
    : occlusionQueries(queries)
    , impl(encoder, targetWidth, targetHeight)
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

        // Encoder state for the same reason, and set to zero by a pipeline
        // without one so a biased shadow caster lends nothing to what follows.
        const auto bias = pipeline.depthBias();
        [activeEncoder setDepthBias:bias.constant
                         slopeScale:bias.slopeScale
                              clamp:bias.clamp];
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
    auto metalBuffer = metalBufferOf(range);

    // The offset is the range's whole contribution: Metal reads vertex zero
    // at buffer + offset, which is exactly what a slice of an arena is.
    if (activeEncoder != nil && metalBuffer != nil)
        [activeEncoder setVertexBuffer:metalBuffer
                                offset:(NSUInteger) range.offset
                               atIndex:(NSUInteger) index];
}

namespace
{
void bindStageTexture(NSObject<MTLRenderCommandEncoder>* encoder,
                      ShaderStage stage,
                      id<MTLTexture> texture,
                      int slot,
                      void* nativeSampler)
{
    auto metalSampler = (__bridge id<MTLSamplerState>) nativeSampler;

    if (encoder == nil || texture == nil || metalSampler == nil || slot < 0
        || slot >= RenderPass::maxStageTextureSlots)
        return;

    const auto index = (NSUInteger) slot;

    if (stage == ShaderStage::Vertex)
    {
        [encoder setVertexTexture:texture atIndex:index];
        [encoder setVertexSamplerState:metalSampler atIndex:index];
        return;
    }

    [encoder setFragmentTexture:texture atIndex:index];
    [encoder setFragmentSamplerState:metalSampler atIndex:index];
}

// The resolve on a multisampled target and the attachment itself on a
// single-sampled one. A depth2d_ms is not what the shader declared and Metal
// will not bind one to a depth2d, which is why a multisampled target carries a
// single-sampled twin at all - see TextureDescriptor::sampleCount.
id<MTLTexture> sampleableDepthOf(const Texture& renderTarget)
{
    auto resolved =
        (__bridge id<MTLTexture>) renderTarget.nativeResolvedDepthTexture();

    return resolved != nil
               ? resolved
               : (__bridge id<MTLTexture>) renderTarget.nativeDepthTexture();
}

id<MTLTexture> colorTextureOf(const Texture& texture)
{
    return (__bridge id<MTLTexture>) texture.nativeTexture();
}

id<MTLTexture> depthTextureOf(const Texture& renderTarget)
{
    return renderTarget.hasSampleableDepth() ? sampleableDepthOf(renderTarget) : nil;
}

id<MTLTexture> depthSliceOf(const Texture& renderTarget, int slice)
{
    return (__bridge id<MTLTexture>) renderTarget.nativeDepthSliceView(slice);
}
} // namespace

// The state for the sampling the shader declared, not one the texture carries -
// that is what keeps this backend agreeing with D3D12, where the declaration is
// the only thing that can pick a sampler for a codegen shader.
void RenderPass::setFragmentTexture(const Texture& texture,
                                    int slot,
                                    TextureSampling sampling)
{
    bindStageTexture(impl->encoder.get(),
                     ShaderStage::Fragment,
                     colorTextureOf(texture),
                     slot,
                     Device::shared().nativeSampler(sampling));
}

void RenderPass::setFragmentTexture(const Texture& texture,
                                    int slot,
                                    const Sampler& sampler)
{
    bindStageTexture(impl->encoder.get(),
                     ShaderStage::Fragment,
                     colorTextureOf(texture),
                     slot,
                     sampler.nativeSampler());
}

// A depth texture goes on a texture index like any other; what makes it a
// depth2d rather than a texture2d is the declaration the shader was compiled
// with.
void RenderPass::setFragmentDepthTexture(const Texture& renderTarget,
                                         int slot,
                                         TextureSampling sampling)
{
    bindStageTexture(impl->encoder.get(),
                     ShaderStage::Fragment,
                     depthTextureOf(renderTarget),
                     slot,
                     Device::shared().nativeSampler(sampling));
}

void RenderPass::setFragmentDepthTexture(const Texture& renderTarget,
                                         int slot,
                                         const Sampler& sampler)
{
    bindStageTexture(impl->encoder.get(),
                     ShaderStage::Fragment,
                     depthTextureOf(renderTarget),
                     slot,
                     sampler.nativeSampler());
}

void RenderPass::setVertexTexture(const Texture& texture,
                                  int slot,
                                  TextureSampling sampling)
{
    bindStageTexture(impl->encoder.get(),
                     ShaderStage::Vertex,
                     colorTextureOf(texture),
                     slot,
                     Device::shared().nativeSampler(sampling));
}

void RenderPass::setVertexTexture(const Texture& texture,
                                  int slot,
                                  const Sampler& sampler)
{
    bindStageTexture(impl->encoder.get(),
                     ShaderStage::Vertex,
                     colorTextureOf(texture),
                     slot,
                     sampler.nativeSampler());
}

void RenderPass::setVertexDepthTexture(const Texture& renderTarget,
                                       int slot,
                                       TextureSampling sampling)
{
    bindStageTexture(impl->encoder.get(),
                     ShaderStage::Vertex,
                     depthTextureOf(renderTarget),
                     slot,
                     Device::shared().nativeSampler(sampling));
}

void RenderPass::setVertexDepthTexture(const Texture& renderTarget,
                                       int slot,
                                       const Sampler& sampler)
{
    bindStageTexture(impl->encoder.get(),
                     ShaderStage::Vertex,
                     depthTextureOf(renderTarget),
                     slot,
                     sampler.nativeSampler());
}

void RenderPass::setFragmentDepthTextureSlice(const Texture& renderTarget,
                                              int slice,
                                              int slot,
                                              TextureSampling sampling)
{
    bindStageTexture(impl->encoder.get(),
                     ShaderStage::Fragment,
                     depthSliceOf(renderTarget, slice),
                     slot,
                     Device::shared().nativeSampler(sampling));
}

void RenderPass::setFragmentDepthTextureSlice(const Texture& renderTarget,
                                              int slice,
                                              int slot,
                                              const Sampler& sampler)
{
    bindStageTexture(impl->encoder.get(),
                     ShaderStage::Fragment,
                     depthSliceOf(renderTarget, slice),
                     slot,
                     sampler.nativeSampler());
}

void RenderPass::setVertexDepthTextureSlice(const Texture& renderTarget,
                                              int slice,
                                              int slot,
                                              TextureSampling sampling)
{
    bindStageTexture(impl->encoder.get(),
                     ShaderStage::Vertex,
                     depthSliceOf(renderTarget, slice),
                     slot,
                     Device::shared().nativeSampler(sampling));
}

void RenderPass::setVertexDepthTextureSlice(const Texture& renderTarget,
                                              int slice,
                                              int slot,
                                              const Sampler& sampler)
{
    bindStageTexture(impl->encoder.get(),
                     ShaderStage::Vertex,
                     depthSliceOf(renderTarget, slice),
                     slot,
                     sampler.nativeSampler());
}

void RenderPass::setVertexStorageBuffer(const Buffer& buffer, int slot)
{
    setVertexStorageBuffer(BufferRange::of(buffer), slot);
}

void RenderPass::setVertexStorageBuffer(const BufferRange& range, int slot)
{
    auto activeEncoder = impl->encoder.get();
    auto metalBuffer = metalBufferOf(range);

    if (activeEncoder != nil && metalBuffer != nil)
        [activeEncoder setVertexBuffer:metalBuffer
                                offset:(NSUInteger) range.offset
                               atIndex:(NSUInteger) (bufferBase + slot)];
}

void RenderPass::setFragmentStorageBuffer(const Buffer& buffer, int slot)
{
    setFragmentStorageBuffer(BufferRange::of(buffer), slot);
}

void RenderPass::setFragmentStorageBuffer(const BufferRange& range, int slot)
{
    auto activeEncoder = impl->encoder.get();
    auto metalBuffer = metalBufferOf(range);

    if (activeEncoder != nil && metalBuffer != nil)
        [activeEncoder setFragmentBuffer:metalBuffer
                                  offset:(NSUInteger) range.offset
                                 atIndex:(NSUInteger) (bufferBase + slot)];
}

void RenderPass::setVertexBytes(const void* data, int bytes, int slot)
{
    // Uniforms live at buffer(uniformBase + slot) so multi-slot vertex
    // layouts (e.g. instancing with slots 0..N) never collide with the
    // uniform bind. Matches ComputePass::uniformBase.
    if (auto activeEncoder = impl->encoder.get())
        [activeEncoder setVertexBytes:data
                               length:(NSUInteger) bytes
                              atIndex:(NSUInteger) (uniformBase + slot)];
}

void RenderPass::setFragmentBytes(const void* data, int bytes, int slot)
{
    // Same uniformBase mapping as the vertex stage, so one slot rule covers
    // both; the generated fragment functions declare the block at
    // buffer(uniformBase).
    if (auto activeEncoder = impl->encoder.get())
        [activeEncoder setFragmentBytes:data
                                 length:(NSUInteger) bytes
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
    auto metalBuffer = metalBufferOf(indices);

    if (! impl->pipelineBound || activeEncoder == nil || metalBuffer == nil)
        return;

    auto indexType = format == IndexFormat::UInt16 ? MTLIndexTypeUInt16
                                                   : MTLIndexTypeUInt32;
    auto indexSize = format == IndexFormat::UInt16 ? (int) sizeof(std::uint16_t)
                                                   : (int) sizeof(std::uint32_t);

    // The eight-argument selector rather than the five-argument one because
    // only this form carries a base vertex; instanceCount:1 makes it the same
    // draw the short form issues. The range's offset and firstIndex are the
    // same thing to Metal - a byte offset into the buffer - so they add.
    [activeEncoder drawIndexedPrimitives:impl->primitiveType
                              indexCount:(NSUInteger) indexCount
                               indexType:indexType
                             indexBuffer:metalBuffer
                       indexBufferOffset:(NSUInteger) (indices.offset
                                                       + firstIndex * indexSize)
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
    auto metalBuffer = metalBufferOf(indices);

    if (! impl->pipelineBound || activeEncoder == nil || metalBuffer == nil)
        return;

    auto indexType = format == IndexFormat::UInt16 ? MTLIndexTypeUInt16
                                                   : MTLIndexTypeUInt32;
    auto indexSize = format == IndexFormat::UInt16 ? (int) sizeof(std::uint16_t)
                                                   : (int) sizeof(std::uint32_t);

    [activeEncoder drawIndexedPrimitives:impl->primitiveType
                              indexCount:(NSUInteger) indexCount
                               indexType:indexType
                             indexBuffer:metalBuffer
                       indexBufferOffset:(NSUInteger) (indices.offset
                                                       + firstIndex * indexSize)
                           instanceCount:(NSUInteger) instanceCount
                              baseVertex:(NSInteger) baseVertex
                            baseInstance:(NSUInteger) firstInstance];
}

void RenderPass::drawIndirect(const Buffer& arguments,
                              std::int64_t offset,
                              int drawCount,
                              std::int64_t stride)
{
    auto plan = planIndirectDraws(
        arguments, offset, drawCount, stride, sizeof(DrawArguments));
    auto activeEncoder = impl->encoder.get();
    auto metalArguments = (__bridge id<MTLBuffer>) arguments.nativeBuffer();

    if (!plan || !impl->pipelineBound || activeEncoder == nil
        || metalArguments == nil)
        return;

    for (auto draw = 0; draw < plan->drawCount; ++draw)
        [activeEncoder drawPrimitives:impl->primitiveType
                       indirectBuffer:metalArguments
                 indirectBufferOffset:(NSUInteger) (plan->offset
                                                    + draw * plan->stride)];
}

void RenderPass::drawIndirectCount(const Buffer&,
                                   std::int64_t,
                                   const Buffer&,
                                   std::int64_t,
                                   int,
                                   std::int64_t)
{
    traceIndirectCountUnsupported();
}

void RenderPass::drawIndexedIndirect(const BufferRange& indices,
                                     IndexFormat format,
                                     const Buffer& arguments,
                                     std::int64_t offset,
                                     int drawCount,
                                     std::int64_t stride)
{
    auto plan = planIndirectDraws(
        arguments, offset, drawCount, stride, sizeof(DrawIndexedArguments));
    auto activeEncoder = impl->encoder.get();
    auto metalArguments = (__bridge id<MTLBuffer>) arguments.nativeBuffer();
    auto metalIndices = metalBufferOf(indices);

    if (!plan || !impl->pipelineBound || activeEncoder == nil
        || metalArguments == nil || metalIndices == nil)
        return;

    auto indexType = format == IndexFormat::UInt16 ? MTLIndexTypeUInt16
                                                   : MTLIndexTypeUInt32;

    for (auto draw = 0; draw < plan->drawCount; ++draw)
        [activeEncoder drawIndexedPrimitives:impl->primitiveType
                                   indexType:indexType
                                 indexBuffer:metalIndices
                           indexBufferOffset:(NSUInteger) indices.offset
                              indirectBuffer:metalArguments
                        indirectBufferOffset:(NSUInteger) (plan->offset
                                                           + draw * plan->stride)];
}

void RenderPass::drawIndexedIndirectCount(const BufferRange&,
                                          IndexFormat,
                                          const Buffer&,
                                          std::int64_t,
                                          const Buffer&,
                                          std::int64_t,
                                          int,
                                          std::int64_t)
{
    traceIndirectCountUnsupported();
}

// A query is a mode set at an offset of the pass's visibility buffer: every
// draw until the mode changes counts into those eight bytes. Disabled is set at
// the same offset rather than at zero, since naming an offset is what makes
// Metal write it - closing at zero would zero query 0 in every pass.
void RenderPass::nativeBeginOcclusionQuery(int index, OcclusionMode mode)
{
    if (auto activeEncoder = impl->encoder.get())
        [activeEncoder
            setVisibilityResultMode:mode == OcclusionMode::AnySamples
                                        ? MTLVisibilityResultModeBoolean
                                        : MTLVisibilityResultModeCounting
                             offset:(NSUInteger) index * sizeof(std::uint64_t)];
}

void RenderPass::nativeEndOcclusionQuery(int index, OcclusionMode)
{
    if (auto activeEncoder = impl->encoder.get())
        [activeEncoder
            setVisibilityResultMode:MTLVisibilityResultModeDisabled
                             offset:(NSUInteger) index * sizeof(std::uint64_t)];
}

void RenderPass::resolveOcclusionQueries()
{
    // Nothing to record: the pass writes the buffer as it ends.
}

void RenderPass::end()
{
    if (impl->ended)
        return;

    // Before endEncoding, so a batching renderer's queued draws still reach
    // this encoder. See RenderPass::Participant.
    drainParticipants();

    // After them, so a participant's draws inside an open query count.
    endOcclusionQuery();

    if (auto activeEncoder = impl->encoder.get())
        [activeEncoder endEncoding];

    impl->ended = true;
}
} // namespace eacp::GPU
