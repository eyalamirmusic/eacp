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
} // namespace eacp::GPU
