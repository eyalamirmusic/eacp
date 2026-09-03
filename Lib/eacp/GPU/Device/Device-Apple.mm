#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

#include "Device.h"

#include <eacp/Core/ObjC/CFRef.h>
#include <eacp/Core/ObjC/ObjC.h>
#include <eacp/Core/Utils/Containers.h>

namespace eacp::GPU
{
namespace
{
MTLSamplerMinMagFilter toMetalFilter(TextureFilter filter)
{
    return filter == TextureFilter::Nearest ? MTLSamplerMinMagFilterNearest
                                            : MTLSamplerMinMagFilterLinear;
}

MTLSamplerAddressMode toMetalAddressMode(TextureAddressMode mode)
{
    return mode == TextureAddressMode::Repeat ? MTLSamplerAddressModeRepeat
                                              : MTLSamplerAddressModeClampToEdge;
}

ObjC::Ptr<NSObject<MTLSamplerState>> makeSampler(id<MTLDevice> metalDevice,
                                                 TextureSampling sampling)
{
    auto samplerDescriptor = ObjC::makePtr<MTLSamplerDescriptor>();
    samplerDescriptor.get().minFilter = toMetalFilter(sampling.filter);
    samplerDescriptor.get().magFilter = toMetalFilter(sampling.filter);
    samplerDescriptor.get().sAddressMode = toMetalAddressMode(sampling.addressMode);
    samplerDescriptor.get().tAddressMode = toMetalAddressMode(sampling.addressMode);

    // Set rather than left at its default, which is NotMipmapped - "sample level
    // 0, whatever other levels exist".
    //
    // D3D12's static samplers have declared MIN_MAG_MIP_LINEAR and
    // MIN_MAG_MIP_POINT since they were written, so the two backends have
    // disagreed here from the start and nothing could tell: no texture had a
    // second level to sample. The first mipmapped one would have been filtered
    // across levels on Windows and read at full size on Apple, from the same
    // TextureSampling and with no way to see it but the picture.
    //
    // This needs no new sampling configuration, which is why the count stays at
    // four: mip filtering on a single-level texture is what both APIs do anyway,
    // so it is invisible to every texture without a chain.
    samplerDescriptor.get().mipFilter = sampling.filter == TextureFilter::Linear
                                            ? MTLSamplerMipFilterLinear
                                            : MTLSamplerMipFilterNearest;

    return [metalDevice newSamplerStateWithDescriptor:samplerDescriptor.get()];
}
} // namespace

struct Device::Native
{
    Native()
    {
        device = MTLCreateSystemDefaultDevice();

        if (device)
        {
            queue = [device.get() newCommandQueue];

            CVMetalTextureCacheRef cache = nullptr;
            CVMetalTextureCacheCreate(
                kCFAllocatorDefault, nullptr, device.get(), nullptr, &cache);
            textureCache.reset(cache);

            buildSamplers();
        }
    }

    // Every sampling configuration gets its state up front: there are four of
    // them, they are cheap, and building them here keeps nativeSampler() a
    // const lookup that any thread can make without a lazy-init race.
    void buildSamplers()
    {
        for (auto filter : {TextureFilter::Nearest, TextureFilter::Linear})
            for (auto mode : {TextureAddressMode::Clamp, TextureAddressMode::Repeat})
            {
                const auto sampling = TextureSampling {filter, mode};
                samplers[samplingIndex(sampling)] =
                    makeSampler(device.get(), sampling);
            }
    }

    ObjC::Ptr<NSObject<MTLDevice>> device;
    ObjC::Ptr<NSObject<MTLCommandQueue>> queue;
    CFRef<CVMetalTextureCacheRef> textureCache;
    Array<ObjC::Ptr<NSObject<MTLSamplerState>>, samplingConfigurations> samplers;

    // Retained rather than held weakly: the command buffer is autoreleased, and
    // the pool it came from may well have drained by the time a read waits.
    ObjC::Ptr<NSObject<MTLCommandBuffer>> lastSubmitted;
};

Device::Device()
    : impl()
{
}

Device& Device::shared()
{
    static Device instance;
    return instance;
}

bool Device::isValid() const
{
    return impl->device.get() != nil;
}

std::string Device::name() const
{
    if (!isValid())
        return "no Metal device";

    return [[impl->device.get() name] UTF8String];
}

// Metal answers this directly, and answers it for the *texture*: a count it
// takes here is one a render attachment can be created at, which is the whole of
// what a caller wants to know.
bool Device::supportsSampleCount(int count) const
{
    if (count <= 1)
        return true;

    if (!isValid())
        return false;

    return [impl->device.get() supportsTextureSampleCount:(NSUInteger) count] == YES;
}

void* Device::nativeContext() const
{
    // Nothing to hand out: the queue, the texture cache and the samplers are
    // already members of Native, reached through the handles below.
    return nullptr;
}

void* Device::nativeDevice() const
{
    return (__bridge void*) impl->device.get();
}

void* Device::nativeQueue() const
{
    return (__bridge void*) impl->queue.get();
}

void* Device::nativeTextureCache() const
{
    return impl->textureCache.get();
}

void* Device::nativeSampler(TextureSampling sampling) const
{
    return (__bridge void*) impl->samplers[samplingIndex(sampling)].get();
}

void Device::trackSubmittedWork(void* nativeCommandBuffer)
{
    impl->lastSubmitted.reset(
        (__bridge NSObject<MTLCommandBuffer>*) nativeCommandBuffer);
}

void Device::waitForSubmittedWork()
{
    // waitUntilCompleted on a command buffer that already finished returns at
    // once, so this costs nothing when there is nothing outstanding.
    if (auto buffer = impl->lastSubmitted.get())
        [(id<MTLCommandBuffer>) buffer waitUntilCompleted];
}
} // namespace eacp::GPU
