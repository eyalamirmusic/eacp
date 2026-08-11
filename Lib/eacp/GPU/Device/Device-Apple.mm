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

    // Set explicitly: the Metal default is NotMipmapped, which would disagree
    // with D3D12's MIN_MAG_MIP_* static samplers on any mipmapped texture.
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

    // Built up front so nativeSampler() stays a race-free const lookup.
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

    // Retained: the command buffer is autoreleased and its pool may drain
    // before a read waits on it.
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
    if (auto buffer = impl->lastSubmitted.get())
        [(id<MTLCommandBuffer>) buffer waitUntilCompleted];
}
} // namespace eacp::GPU
