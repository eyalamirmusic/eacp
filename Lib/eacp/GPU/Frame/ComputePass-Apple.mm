#import <Metal/Metal.h>

#include "ComputePass.h"

#include "../Buffer/Buffer.h"
#include "../Device/Device.h"
#include "../Pipeline/ComputePipeline.h"

#include <eacp/Core/ObjC/ObjC.h>

namespace eacp::GPU
{
struct ComputePass::Native
{
    Native(void* encoderHandle, DispatchOrder dispatchOrder)
        : order(dispatchOrder)
    {
        if (encoderHandle != nullptr)
            encoder.reset((__bridge NSObject<MTLComputeCommandEncoder>*) encoderHandle);
    }

    ObjC::Ptr<NSObject<MTLComputeCommandEncoder>> encoder;
    DispatchOrder order = DispatchOrder::Serial;
    bool ended = false;
};

ComputePass::ComputePass(void* encoder, DispatchOrder order)
    : impl(encoder, order)
{
}

ComputePass::~ComputePass()
{
    end();
}

void ComputePass::encodePipeline(const ComputePipeline& pipeline)
{
    boundGroup = pipeline.threadGroupShape();

    auto activeEncoder = impl->encoder.get();
    auto state = (__bridge id<MTLComputePipelineState>) pipeline.nativeState();

    boundPipeline = activeEncoder != nil && state != nil;

    if (boundPipeline)
        [activeEncoder setComputePipelineState:state];
}

void ComputePass::setInputBuffer(const Buffer& buffer, int slot)
{
    setInputBuffer(BufferRange::of(buffer), slot);
}

void ComputePass::setInputBuffer(const BufferRange& range, int slot)
{
    auto activeEncoder = impl->encoder.get();

    if (activeEncoder == nil || slot < 0 || slot >= maxBufferSlots
        || !range.isValid() || range.offset < 0
        || range.offset >= range.buffer->size())
        return;

    auto metalBuffer = (__bridge id<MTLBuffer>) range.buffer->nativeBuffer();

    if (metalBuffer == nil)
        return;

    [activeEncoder setBuffer:metalBuffer
                      offset:(NSUInteger) range.offset
                     atIndex:(NSUInteger) slot];
}

void ComputePass::setOutputBuffer(const Buffer& buffer, int slot)
{
    setOutputBuffer(BufferRange::of(buffer), slot);
}

void ComputePass::setOutputBuffer(const BufferRange& range, int slot)
{
    // Metal binds a device buffer the same way whether the kernel reads or
    // writes it; the read/write distinction only matters to D3D's view types.
    setInputBuffer(range, slot);
}

namespace
{
void bindInputTexture(NSObject<MTLComputeCommandEncoder>* encoder,
                      id<MTLTexture> metalTexture,
                      int slot,
                      void* nativeSampler)
{
    auto metalSampler = (__bridge id<MTLSamplerState>) nativeSampler;

    if (encoder == nil || metalTexture == nil || metalSampler == nil)
        return;

    [encoder setTexture:metalTexture atIndex:(NSUInteger) slot];
    [encoder setSamplerState:metalSampler atIndex:(NSUInteger) slot];
}

void bindInputTexture(NSObject<MTLComputeCommandEncoder>* encoder,
                      const Texture& texture,
                      int slot,
                      void* nativeSampler)
{
    bindInputTexture(encoder,
                     (__bridge id<MTLTexture>) texture.nativeTexture(),
                     slot,
                     nativeSampler);
}

void bindOutputTexture(NSObject<MTLComputeCommandEncoder>* encoder,
                       const Texture& texture,
                       id<MTLTexture> metalTexture,
                       int slot)
{
    if (encoder == nil || metalTexture == nil || !texture.isComputeWritable())
        return;

    [encoder setTexture:metalTexture atIndex:(NSUInteger) slot];
}

// The resolve on a multisampled target and the attachment on a single-sampled
// one, exactly as the render pass's depth binds pick it; nil without
// sampleableDepth.
id<MTLTexture> kernelDepthOf(const Texture& renderTarget)
{
    if (!renderTarget.hasSampleableDepth())
        return nil;

    auto resolved =
        (__bridge id<MTLTexture>) renderTarget.nativeResolvedDepthTexture();

    return resolved != nil
               ? resolved
               : (__bridge id<MTLTexture>) renderTarget.nativeDepthTexture();
}
} // namespace

// The state for the sampling the shader declared, not one the texture carries -
// the same rule the render pass follows.
void ComputePass::setInputTexture(const Texture& texture,
                                  int slot,
                                  TextureSampling sampling)
{
    bindInputTexture(impl->encoder.get(),
                     texture,
                     slot,
                     Device::shared().nativeSampler(sampling));
}

void ComputePass::setInputTexture(const Texture& texture,
                                  int slot,
                                  const Sampler& sampler)
{
    bindInputTexture(impl->encoder.get(), texture, slot, sampler.nativeSampler());
}

// Metal binds a texture the same way whether the kernel reads or writes it;
// what separates the two is the usage it was created with and the access
// qualifier the kernel declared. No sampler: a written texture has none.
void ComputePass::setOutputTexture(const Texture& texture, int slot)
{
    bindOutputTexture(impl->encoder.get(),
                      texture,
                      (__bridge id<MTLTexture>) texture.nativeTexture(),
                      slot);
}

void ComputePass::setInputTextureLevel(const Texture& texture,
                                       int slot,
                                       int level,
                                       TextureSampling sampling)
{
    bindInputTexture(impl->encoder.get(),
                     (__bridge id<MTLTexture>) texture.nativeLevelView(level),
                     slot,
                     Device::shared().nativeSampler(sampling));
}

void ComputePass::setOutputTextureLevel(const Texture& texture, int slot, int level)
{
    bindOutputTexture(impl->encoder.get(),
                      texture,
                      (__bridge id<MTLTexture>) texture.nativeLevelView(level),
                      slot);
}

void ComputePass::setInputTextureAsArray(const Texture& texture,
                                         int slot,
                                         int level,
                                         TextureSampling sampling)
{
    bindInputTexture(impl->encoder.get(),
                     (__bridge id<MTLTexture>) texture.nativeArrayView(level),
                     slot,
                     Device::shared().nativeSampler(sampling));
}

void ComputePass::setOutputTextureAsArray(const Texture& texture, int slot, int level)
{
    bindOutputTexture(impl->encoder.get(),
                      texture,
                      (__bridge id<MTLTexture>) texture.nativeArrayView(level),
                      slot);
}

// A depth2d, depth2d_array or depthcube by the declaration the kernel was
// compiled with, as in the render pass.
void ComputePass::setInputDepthTexture(const Texture& renderTarget,
                                       int slot,
                                       TextureSampling sampling)
{
    bindInputTexture(impl->encoder.get(),
                     kernelDepthOf(renderTarget),
                     slot,
                     Device::shared().nativeSampler(sampling));
}

void ComputePass::setInputDepthTexture(const Texture& renderTarget,
                                       int slot,
                                       const Sampler& sampler)
{
    bindInputTexture(impl->encoder.get(),
                     kernelDepthOf(renderTarget),
                     slot,
                     sampler.nativeSampler());
}

void ComputePass::setInputDepthTextureSlice(const Texture& renderTarget,
                                            int slice,
                                            int slot,
                                            TextureSampling sampling)
{
    bindInputTexture(impl->encoder.get(),
                     (__bridge id<MTLTexture>) renderTarget.nativeDepthSliceView(slice),
                     slot,
                     Device::shared().nativeSampler(sampling));
}

void ComputePass::setInputDepthTextureSlice(const Texture& renderTarget,
                                            int slice,
                                            int slot,
                                            const Sampler& sampler)
{
    bindInputTexture(impl->encoder.get(),
                     (__bridge id<MTLTexture>) renderTarget.nativeDepthSliceView(slice),
                     slot,
                     sampler.nativeSampler());
}

void ComputePass::setBytes(const void* data, std::int64_t bytes, int slot)
{
    if (auto activeEncoder = impl->encoder.get())
        [activeEncoder setBytes:data
                         length:(NSUInteger) bytes
                        atIndex:(NSUInteger) (uniformBase + slot)];
}

void ComputePass::encodeDispatch(int count)
{
    auto activeEncoder = impl->encoder.get();

    if (activeEncoder == nil || !boundPipeline || count <= 0)
        return;

    auto group = groupFor1D();
    auto width = (NSUInteger) group.x;
    auto groups = ((NSUInteger) count + width - 1) / width;

    [activeEncoder
         dispatchThreadgroups:MTLSizeMake(groups, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(width,
                                          (NSUInteger) group.y,
                                          (NSUInteger) group.z)];
}

void ComputePass::encodeDispatch(int width, int height)
{
    auto activeEncoder = impl->encoder.get();

    if (activeEncoder == nil || !boundPipeline || width <= 0 || height <= 0)
        return;

    auto group = groupFor2D();
    auto sizeX = (NSUInteger) group.x;
    auto sizeY = (NSUInteger) group.y;
    auto groupsX = ((NSUInteger) width + sizeX - 1) / sizeX;
    auto groupsY = ((NSUInteger) height + sizeY - 1) / sizeY;

    [activeEncoder
         dispatchThreadgroups:MTLSizeMake(groupsX, groupsY, 1)
        threadsPerThreadgroup:MTLSizeMake(sizeX, sizeY, (NSUInteger) group.z)];
}

void ComputePass::encodeDispatch(int width, int height, int depth)
{
    auto activeEncoder = impl->encoder.get();

    if (activeEncoder == nil || !boundPipeline || width <= 0 || height <= 0
        || depth <= 0)
        return;

    auto group = groupFor3D();
    auto sizeX = (NSUInteger) group.x;
    auto sizeY = (NSUInteger) group.y;
    auto sizeZ = (NSUInteger) group.z;
    auto groupsX = ((NSUInteger) width + sizeX - 1) / sizeX;
    auto groupsY = ((NSUInteger) height + sizeY - 1) / sizeY;
    auto groupsZ = ((NSUInteger) depth + sizeZ - 1) / sizeZ;

    [activeEncoder dispatchThreadgroups:MTLSizeMake(groupsX, groupsY, groupsZ)
                  threadsPerThreadgroup:MTLSizeMake(sizeX, sizeY, sizeZ)];
}

// The threadgroup size still comes from here - only the *count* is in the
// buffer. Metal reads three uint32s at the offset, which is what
// DispatchArguments is, so no conversion happens on the way.
void ComputePass::encodeDispatchIndirect(const Buffer& arguments,
                                   std::int64_t offsetInBytes)
{
    auto activeEncoder = impl->encoder.get();
    auto metalBuffer = (__bridge id<MTLBuffer>) arguments.nativeBuffer();

    if (activeEncoder == nil || !boundPipeline || metalBuffer == nil
        || offsetInBytes < 0
        || offsetInBytes
               > arguments.size() - (std::int64_t) sizeof(DispatchArguments))
        return;

    auto group = groupFor1D();

    [activeEncoder dispatchThreadgroupsWithIndirectBuffer:metalBuffer
                                    indirectBufferOffset:(NSUInteger) offsetInBytes
                                   threadsPerThreadgroup:MTLSizeMake(
                                                             (NSUInteger) group.x,
                                                             (NSUInteger) group.y,
                                                             (NSUInteger) group.z)];
}

void ComputePass::encodeBarrier()
{
    if (impl->order != DispatchOrder::Concurrent)
        return;

    if (auto activeEncoder = impl->encoder.get())
        [activeEncoder memoryBarrierWithScope:MTLBarrierScopeBuffers
                                              | MTLBarrierScopeTextures];
}

// The encoder's own end orders everything it recorded against whatever the
// command buffer does next, concurrent dispatch included, so there is no closing
// barrier to record here.
void ComputePass::end()
{
    if (impl->ended)
        return;

    if (auto activeEncoder = impl->encoder.get())
        [activeEncoder endEncoding];

    impl->ended = true;
}
} // namespace eacp::GPU
