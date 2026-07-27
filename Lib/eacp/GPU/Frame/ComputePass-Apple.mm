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
    explicit Native(void* encoderHandle)
    {
        if (encoderHandle != nullptr)
            encoder.reset((__bridge NSObject<MTLComputeCommandEncoder>*) encoderHandle);
    }

    ObjC::Ptr<NSObject<MTLComputeCommandEncoder>> encoder;
    bool ended = false;
};

ComputePass::ComputePass(void* encoder)
    : impl(encoder)
{
}

ComputePass::~ComputePass()
{
    end();
}

void ComputePass::setPipeline(const ComputePipeline& pipeline)
{
    auto activeEncoder = impl->encoder.get();
    auto state = (__bridge id<MTLComputePipelineState>) pipeline.nativeState();

    if (activeEncoder != nil && state != nil)
        [activeEncoder setComputePipelineState:state];
}

void ComputePass::setInputBuffer(const Buffer& buffer, int slot)
{
    auto activeEncoder = impl->encoder.get();
    auto metalBuffer = (__bridge id<MTLBuffer>) buffer.nativeBuffer();

    if (activeEncoder != nil && metalBuffer != nil)
        [activeEncoder setBuffer:metalBuffer offset:0 atIndex:(NSUInteger) slot];
}

void ComputePass::setOutputBuffer(const Buffer& buffer, int slot)
{
    // Metal binds a device buffer the same way whether the kernel reads or
    // writes it; the read/write distinction only matters to D3D's view types.
    setInputBuffer(buffer, slot);
}

void ComputePass::setInputTexture(const Texture& texture,
                                  int slot,
                                  TextureSampling sampling)
{
    auto activeEncoder = impl->encoder.get();
    auto metalTexture = (__bridge id<MTLTexture>) texture.nativeTexture();

    // The state for the sampling the shader declared, not one the texture
    // carries - the same rule the render pass follows, and the one D3D12's
    // static samplers leave no alternative to.
    auto metalSampler =
        (__bridge id<MTLSamplerState>) Device::shared().nativeSampler(sampling);

    if (activeEncoder == nil || metalTexture == nil || metalSampler == nil)
        return;

    [activeEncoder setTexture:metalTexture atIndex:(NSUInteger) slot];
    [activeEncoder setSamplerState:metalSampler atIndex:(NSUInteger) slot];
}

void ComputePass::setOutputTexture(const Texture& texture, int slot)
{
    auto activeEncoder = impl->encoder.get();
    auto metalTexture = (__bridge id<MTLTexture>) texture.nativeTexture();

    if (activeEncoder == nil || metalTexture == nil || !texture.isComputeWritable())
        return;

    // Metal binds a texture the same way whether the kernel reads or writes it;
    // what separates the two is the usage it was created with and the access
    // qualifier the kernel declared. No sampler: a written texture has none.
    [activeEncoder setTexture:metalTexture atIndex:(NSUInteger) slot];
}

void ComputePass::setBytes(const void* data, std::size_t bytes, int slot)
{
    if (auto activeEncoder = impl->encoder.get())
        [activeEncoder setBytes:data
                         length:bytes
                        atIndex:(NSUInteger) (uniformBase + slot)];
}

void ComputePass::dispatch(int count)
{
    auto activeEncoder = impl->encoder.get();

    if (activeEncoder == nil || count <= 0)
        return;

    auto width = (NSUInteger) threadGroupWidth;
    auto groups = ((NSUInteger) count + width - 1) / width;

    [activeEncoder dispatchThreadgroups:MTLSizeMake(groups, 1, 1)
                  threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
}

void ComputePass::dispatch(int width, int height)
{
    auto activeEncoder = impl->encoder.get();

    if (activeEncoder == nil || width <= 0 || height <= 0)
        return;

    auto size = (NSUInteger) threadGroupSize2D;
    auto groupsX = ((NSUInteger) width + size - 1) / size;
    auto groupsY = ((NSUInteger) height + size - 1) / size;

    [activeEncoder dispatchThreadgroups:MTLSizeMake(groupsX, groupsY, 1)
                  threadsPerThreadgroup:MTLSizeMake(size, size, 1)];
}

void ComputePass::end()
{
    if (impl->ended)
        return;

    if (auto activeEncoder = impl->encoder.get())
        [activeEncoder endEncoding];

    impl->ended = true;
}
} // namespace eacp::GPU
