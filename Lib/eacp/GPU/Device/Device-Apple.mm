#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

#include "Device.h"

#include "../Frame/ComputePass.h"
#include "../Frame/RenderPass.h"

#include <eacp/Core/ObjC/CFRef.h>
#include <eacp/Core/ObjC/ObjC.h>
#include <eacp/Core/Utils/Containers.h>
#include <eacp/Core/Utils/Environment.h>

#include <algorithm>

namespace eacp::GPU
{
namespace
{
MTLSamplerMinMagFilter toMetalFilter(SamplerFilter filter)
{
    return filter == SamplerFilter::Nearest ? MTLSamplerMinMagFilterNearest
                                            : MTLSamplerMinMagFilterLinear;
}

MTLSamplerMipFilter toMetalMipFilter(SamplerMipFilter filter)
{
    switch (filter)
    {
        case SamplerMipFilter::None:
            return MTLSamplerMipFilterNotMipmapped;
        case SamplerMipFilter::Nearest:
            return MTLSamplerMipFilterNearest;
        case SamplerMipFilter::Linear:
            return MTLSamplerMipFilterLinear;
    }

    return MTLSamplerMipFilterNotMipmapped;
}

MTLSamplerAddressMode toMetalAddressMode(SamplerAddressMode mode)
{
    switch (mode)
    {
        case SamplerAddressMode::ClampToEdge:
            return MTLSamplerAddressModeClampToEdge;
        case SamplerAddressMode::Repeat:
            return MTLSamplerAddressModeRepeat;
        case SamplerAddressMode::MirrorRepeat:
            return MTLSamplerAddressModeMirrorRepeat;
        case SamplerAddressMode::ClampToBorder:
            return MTLSamplerAddressModeClampToBorderColor;
    }

    return MTLSamplerAddressModeClampToEdge;
}

MTLSamplerBorderColor toMetalBorderColor(SamplerBorderColor color)
{
    switch (color)
    {
        case SamplerBorderColor::TransparentBlack:
            return MTLSamplerBorderColorTransparentBlack;
        case SamplerBorderColor::OpaqueBlack:
            return MTLSamplerBorderColorOpaqueBlack;
        case SamplerBorderColor::OpaqueWhite:
            return MTLSamplerBorderColorOpaqueWhite;
    }

    return MTLSamplerBorderColorTransparentBlack;
}

MTLCompareFunction toMetalCompareFunction(CompareFunction compare)
{
    switch (compare)
    {
        case CompareFunction::Never:
            return MTLCompareFunctionNever;
        case CompareFunction::Less:
            return MTLCompareFunctionLess;
        case CompareFunction::LessEqual:
            return MTLCompareFunctionLessEqual;
        case CompareFunction::Equal:
            return MTLCompareFunctionEqual;
        case CompareFunction::NotEqual:
            return MTLCompareFunctionNotEqual;
        case CompareFunction::GreaterEqual:
            return MTLCompareFunctionGreaterEqual;
        case CompareFunction::Greater:
            return MTLCompareFunctionGreater;
        case CompareFunction::Always:
            return MTLCompareFunctionAlways;
    }

    return MTLCompareFunctionNever;
}

// The four TextureSampling configurations come through here too, as
// SamplerDescriptor::from(sampling): mip filtering follows the min/mag filter
// rather than Metal's NotMipmapped default, which is what D3D12's static
// samplers have always declared.
ObjC::Ptr<NSObject<MTLSamplerState>>
    makeMetalSampler(id<MTLDevice> metalDevice, const SamplerDescriptor& descriptor)
{
    auto samplerDescriptor = ObjC::makePtr<MTLSamplerDescriptor>();
    auto* metal = samplerDescriptor.get();

    metal.minFilter = toMetalFilter(descriptor.minFilter);
    metal.magFilter = toMetalFilter(descriptor.magFilter);
    metal.mipFilter = toMetalMipFilter(descriptor.mipFilter);
    metal.sAddressMode = toMetalAddressMode(descriptor.addressU);
    metal.tAddressMode = toMetalAddressMode(descriptor.addressV);
    metal.rAddressMode = toMetalAddressMode(descriptor.addressW);
    metal.borderColor = toMetalBorderColor(descriptor.borderColor);
    metal.lodMinClamp = descriptor.lodMinClamp;
    metal.lodMaxClamp = descriptor.lodMaxClamp;

    if (descriptor.isFullyLinear())
        metal.maxAnisotropy =
            (NSUInteger) std::clamp(descriptor.maxAnisotropy, 1, 16);

    if (descriptor.compare)
        metal.compareFunction = toMetalCompareFunction(*descriptor.compare);

    return [metalDevice newSamplerStateWithDescriptor:metal];
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
        for (auto filter: {TextureFilter::Nearest, TextureFilter::Linear})
            for (auto mode: {TextureAddressMode::Clamp, TextureAddressMode::Repeat})
            {
                const auto sampling = TextureSampling {filter, mode};
                samplers[samplingIndex(sampling)] =
                    makeMetalSampler(device.get(), SamplerDescriptor::from(sampling));
            }
    }

    ObjC::Ptr<NSObject<MTLDevice>> device;
    ObjC::Ptr<NSObject<MTLCommandQueue>> queue;
    CFRef<CVMetalTextureCacheRef> textureCache;
    Array<ObjC::Ptr<NSObject<MTLSamplerState>>, samplingConfigurations> samplers;

    // Every other sampler Device::makeSampler made, kept for the device's
    // lifetime because a Sampler handle does not own its state.
    Vector<ObjC::Ptr<NSObject<MTLSamplerState>>> describedSamplers;

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

    // Created lazily but owned by the main thread whichever thread asked for it
    // first — every GPUView and every Frame drives this one from there. See the
    // thread rule on Device.
    [[maybe_unused]] static const auto boundToMainThread =
        (instance.followMainThread(), true);

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
// 15.0 - so the guard is inert on macOS as configured and real on iOS.
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

bool Device::supportsIndirectCount() const
{
    return false;
}

// Visibility results are on every Metal device, counting included.
bool Device::supportsOcclusionQueries() const
{
    return isValid();
}

bool Device::supportsOcclusionSampleCounts() const
{
    return isValid();
}

int Device::maxTextureSlots() const
{
    return isValid() ? RenderPass::maxStageTextureSlots : 0;
}

bool Device::supportsVertexTextures() const
{
    return isValid();
}

bool Device::stagesShareTextureSlots() const
{
    return false;
}

int Device::maxTextureLayers() const
{
    return isValid() ? 2048 : 0;
}

int Device::maxTextureSize3D() const
{
    return isValid() ? 2048 : 0;
}

bool Device::supportsCubeArrays() const
{
    auto metalDevice = (__bridge id<MTLDevice>) nativeDevice();

    if (metalDevice == nil)
        return false;

    return [metalDevice supportsFamily:MTLGPUFamilyMac2] == YES
           || [metalDevice supportsFamily:MTLGPUFamilyApple4] == YES;
}

bool Device::supportsLayeredRendering() const
{
    auto metalDevice = (__bridge id<MTLDevice>) nativeDevice();

    if (metalDevice == nil)
        return false;

    return [metalDevice supportsFamily:MTLGPUFamilyMac2] == YES
           || [metalDevice supportsFamily:MTLGPUFamilyApple5] == YES;
}

// BGRA8 by policy (G79: the hardware can, the other APIs cannot), sRGB because
// Metal has no shader write that encodes, RGB9E5Float because the Mac2 family
// cannot and the other APIs never can.
bool Device::supportsComputeWrite(TextureFormat format) const
{
    return isValid() && isRenderableFormat(format) && !isSrgbFormat(format)
           && format != TextureFormat::BGRA8Unorm;
}

namespace
{
// Metal's own lists: tier 1 is the three 32-bit single-channel formats, tier 2
// adds the four-channel 8-, 16- and 32-bit ones and the one-channel 8- and
// 16-bit ones, float, unorm and integer alike - and no two-channel format.
bool readWritesAtTierOne(TextureFormat format)
{
    return format == TextureFormat::R32Float || format == TextureFormat::R32Uint
           || format == TextureFormat::R32Sint;
}

bool readWritesAtTierTwo(TextureFormat format)
{
    switch (format)
    {
        case TextureFormat::RGBA8Unorm:
        case TextureFormat::RGBA8Uint:
        case TextureFormat::RGBA8Sint:
        case TextureFormat::RGBA16Float:
        case TextureFormat::RGBA16Uint:
        case TextureFormat::RGBA16Sint:
        case TextureFormat::RGBA32Float:
        case TextureFormat::RGBA32Uint:
        case TextureFormat::RGBA32Sint:
        case TextureFormat::R16Float:
        case TextureFormat::R16Uint:
        case TextureFormat::R16Sint:
        case TextureFormat::R8Unorm:
        case TextureFormat::R8Uint:
        case TextureFormat::R8Sint:
            return true;
        default:
            return readWritesAtTierOne(format);
    }
}
} // namespace

bool Device::supportsComputeReadWrite(TextureFormat format) const
{
    auto metalDevice = (__bridge id<MTLDevice>) nativeDevice();

    if (metalDevice == nil || !supportsComputeWrite(format))
        return false;

    switch (metalDevice.readWriteTextureSupport)
    {
        case MTLReadWriteTextureTier2:
            return readWritesAtTierTwo(format);
        case MTLReadWriteTextureTier1:
            return readWritesAtTierOne(format);
        case MTLReadWriteTextureTierNone:
            break;
    }

    return false;
}

bool Device::supportsMipGeneration(TextureFormat format) const
{
    auto metalDevice = (__bridge id<MTLDevice>) nativeDevice();

    // generateMipmapsForTexture wants a format it can render into, which
    // RGB9E5Float is only on Apple GPUs; its chain is the CPU's everywhere.
    if (metalDevice == nil || !isRenderableFormat(format) || isIntegerFormat(format))
        return false;

    const auto fullFloat = format == TextureFormat::R32Float
                           || format == TextureFormat::RG32Float
                           || format == TextureFormat::RGBA32Float;

    return !fullFloat || metalDevice.supports32BitFloatFiltering;
}

int Device::maxComputeBufferSlots() const
{
    return isValid() ? ComputePass::maxBufferSlots : 0;
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
// macOS 14 and iOS 17, and eacp's deployment targets are 11.0 and 15.0. The
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

void* Device::createNativeSampler(const SamplerDescriptor& descriptor)
{
    auto sampler = makeMetalSampler(impl->device.get(), descriptor);

    if (sampler.get() == nil)
        return nullptr;

    auto* native = (__bridge void*) sampler.get();
    impl->describedSamplers.add(std::move(sampler));
    return native;
}

bool Device::supportsPerBindSamplers() const
{
    return isValid();
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
