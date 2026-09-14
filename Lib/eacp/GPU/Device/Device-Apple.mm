#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

#include "Device.h"

#include <eacp/Core/ObjC/CFRef.h>
#include <eacp/Core/ObjC/ObjC.h>
#include <eacp/Core/Utils/Containers.h>
#include <eacp/Core/Utils/Environment.h>

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

// One property, and the guard around it is the whole of the platform story: the
// query is macOS 11 and iOS 16.4, and eacp's deployment targets are 11.0 and
// 14.0 - so the guard is inert on macOS as configured and real on iOS.
//
// The false below is not a claim about the hardware. On iOS it is very nearly
// one, BC having reached the platform in the same release the query did; on a
// macOS older than 11 the formats are there (Metal has had them since 10.11)
// and there is simply no way to ask, so eacp declines rather than guesses. See
// Device::supportsBlockCompression.
bool Device::supportsBlockCompression() const
{
    if (!isValid())
        return false;

    if (@available(macOS 11.0, iOS 16.4, *))
        return [impl->device.get() supportsBCTextureCompression] == YES;

    return false;
}

// Nothing to ask the device. Metal aligns a setBuffer offset to four bytes on
// every GPU eacp runs on, so the grid is the API's own and a range on it binds
// here whatever the hardware. Vulkan is where this varies, which is why it is a
// Device call rather than a constant.
int Device::storageBufferOffsetAlignment() const
{
    return 4;
}

int Device::maxThreadgroupMemory() const
{
    auto metalDevice = (__bridge id<MTLDevice>) nativeDevice();

    if (metalDevice == nil)
        return 0;

    return (int) metalDevice.maxThreadgroupMemoryLength;
}

// The family is the gate both packed fragment types share. MTLGPUFamilyApple7
// is the first with the SIMD-group matrix instructions, and it is also where
// the SIMD group is the 32 threads the EDSL's fragment layout is written
// against - an Intel Mac has neither, and answering no there is a claim about
// this GPU rather than about the OS.
//
// EACP_NO_PACKED_SIMD_MATRIX takes the answer away on a machine that has it, so
// the staged path both queries exist to select stays reachable in a test on
// hardware that would otherwise never take it.
bool Device::supportsHalfSimdMatrix() const
{
    if (!isValid() || getEnvValue("EACP_NO_PACKED_SIMD_MATRIX") == "1")
        return false;

    auto metalDevice = (__bridge id<MTLDevice>) nativeDevice();

    return [metalDevice supportsFamily:MTLGPUFamilyApple7] == YES;
}

// Everything above plus the OS: simdgroup_bfloat8x8 is Metal 3.1, which is
// macOS 14 and iOS 17, and eacp's deployment targets are 11.0 and 14.0. The
// guard is therefore real on both platforms rather than inert on one.
//
// Known risk, stated rather than hidden: this pairs the OS with Apple7, and
// Apple7 is an M1. Metal 3.1 is a *language* version, so the type exists
// wherever the OS is new enough, but whether every Apple7 part has the bf16
// matrix instruction under it was measured here on an Apple9 only - an M1 on
// macOS 14 will answer yes to this and has not been checked. Should such a part
// turn out not to have it, the shader fails to compile, and the whole of what
// that costs is the quiet refusal ComputeProgram::prepare already makes: an
// invalid library, an invalid pipeline, and a dispatch that ComputePass drops.
// Nothing crashes and nothing silently computes a wrong answer. Narrow this to
// a later family, or to a runtime compile probe, the moment such a device is
// found.
bool Device::supportsBFloat16SimdMatrix() const
{
    if (!supportsHalfSimdMatrix())
        return false;

    if (@available(macOS 14.0, iOS 17.0, *))
        return true;

    return false;
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
