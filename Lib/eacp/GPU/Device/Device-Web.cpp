#include "Device.h"

#include "../Frame/ComputePass.h"
#include "../Frame/RenderPass.h"
#include "../WebGPU/WebGPUContext.h"

#include <algorithm>

namespace eacp::GPU
{
struct Device::Native
{
    Vector<wgpu::Sampler> samplers;

    // Mutable because the accessors that reach it are const, and a const Device
    // still submits.
    mutable WebGPUContext context;
};

Device::Device()
    : impl()
{
}

Device& Device::shared()
{
    static Device instance;

    [[maybe_unused]] static const auto boundToMainThread =
        (instance.followMainThread(), true);

    return instance;
}

bool Device::isValid() const
{
    return impl->context.isValid();
}

std::string Device::name() const
{
    if (!isValid())
        return "no WebGPU device";

    return impl->context.getAdapterName();
}

bool Device::supportsSampleCount(int count) const
{
    return isValid() && (count == 1 || count == 4);
}

bool Device::supportsBlockCompression() const
{
    return impl->context.hasFeature(wgpu::FeatureName::TextureCompressionBC);
}

int Device::storageBufferOffsetAlignment() const
{
    if (!isValid())
        return 4;

    return static_cast<int>(
        impl->context.getLimits().minStorageBufferOffsetAlignment);
}

bool Device::supportsIndirectCount() const
{
    return false;
}

bool Device::supportsOcclusionQueries() const
{
    return false;
}

bool Device::supportsOcclusionSampleCounts() const
{
    return false;
}

int Device::maxTextureSlots() const
{
    if (!isValid())
        return 0;

    const auto& limits = impl->context.getLimits();
    const auto perStage = std::min(limits.maxSampledTexturesPerShaderStage,
                                   limits.maxSamplersPerShaderStage);

    return std::min(RenderPass::maxStageTextureSlots, static_cast<int>(perStage));
}

bool Device::supportsVertexTextures() const
{
    return isValid();
}

bool Device::stagesShareTextureSlots() const
{
    return true;
}

int Device::maxComputeBufferSlots() const
{
    if (!isValid())
        return 0;

    const auto perStage =
        static_cast<int>(impl->context.getLimits().maxStorageBuffersPerShaderStage);

    return std::min(ComputePass::maxBufferSlots, perStage);
}

int Device::maxTextureLayers() const
{
    if (!isValid())
        return 0;

    return static_cast<int>(impl->context.getLimits().maxTextureArrayLayers);
}

int Device::maxTextureSize3D() const
{
    if (!isValid())
        return 0;

    return static_cast<int>(impl->context.getLimits().maxTextureDimension3D);
}

bool Device::supportsCubeArrays() const
{
    return isValid();
}

bool Device::supportsLayeredRendering() const
{
    return false;
}

// The formats WebGPU's core storage textures take, and the ones the Tier 1
// formats feature adds where the device has it.
bool Device::supportsComputeWrite(TextureFormat format) const
{
    if (!isValid())
        return false;

    switch (format)
    {
        case TextureFormat::RGBA8Unorm:
        case TextureFormat::RGBA8Uint:
        case TextureFormat::RGBA8Sint:
        case TextureFormat::RGBA16Uint:
        case TextureFormat::RGBA16Sint:
        case TextureFormat::RGBA16Float:
        case TextureFormat::R32Float:
        case TextureFormat::R32Uint:
        case TextureFormat::R32Sint:
        case TextureFormat::RG32Float:
        case TextureFormat::RG32Uint:
        case TextureFormat::RG32Sint:
        case TextureFormat::RGBA32Float:
        case TextureFormat::RGBA32Uint:
        case TextureFormat::RGBA32Sint:
            return true;
        case TextureFormat::R8Unorm:
        case TextureFormat::RG8Unorm:
        case TextureFormat::R16Float:
        case TextureFormat::RG16Float:
        case TextureFormat::R8Uint:
        case TextureFormat::RG8Uint:
        case TextureFormat::R16Uint:
        case TextureFormat::RG16Uint:
        case TextureFormat::R8Sint:
        case TextureFormat::RG8Sint:
        case TextureFormat::R16Sint:
        case TextureFormat::RG16Sint:
        case TextureFormat::R16Unorm:
        case TextureFormat::RG16Unorm:
        case TextureFormat::RGBA16Unorm:
        case TextureFormat::RGB10A2Unorm:
        case TextureFormat::RG11B10Float:
            return impl->context.hasFeature(wgpu::FeatureName::TextureFormatsTier1);
        default:
            return false;
    }
}

bool Device::supportsComputeReadWrite(TextureFormat format) const
{
    if (!isValid())
        return false;

    switch (format)
    {
        case TextureFormat::R32Float:
        case TextureFormat::R32Uint:
        case TextureFormat::R32Sint:
            return true;
        case TextureFormat::R8Unorm:
        case TextureFormat::R8Uint:
        case TextureFormat::R8Sint:
        case TextureFormat::RGBA8Unorm:
        case TextureFormat::RGBA8Uint:
        case TextureFormat::RGBA8Sint:
        case TextureFormat::R16Uint:
        case TextureFormat::R16Sint:
        case TextureFormat::R16Float:
        case TextureFormat::RGBA16Uint:
        case TextureFormat::RGBA16Sint:
        case TextureFormat::RGBA16Float:
        case TextureFormat::RGBA32Uint:
        case TextureFormat::RGBA32Sint:
        case TextureFormat::RGBA32Float:
            return impl->context.hasFeature(wgpu::FeatureName::TextureFormatsTier2);
        default:
            return false;
    }
}

bool Device::supportsShaderFloat16() const
{
    return impl->context.hasFeature(wgpu::FeatureName::ShaderF16);
}

bool Device::supportsSampleRateShading() const
{
    return isValid();
}

bool Device::supportsMipGeneration(TextureFormat) const
{
    return false;
}

int Device::maxThreadgroupMemory() const
{
    if (!isValid())
        return 0;

    return static_cast<int>(
        impl->context.getLimits().maxComputeWorkgroupStorageSize);
}

bool Device::supportsHalfSimdMatrix() const
{
    return false;
}

bool Device::supportsBFloat16SimdMatrix() const
{
    return false;
}

void* Device::nativeContext() const
{
    return &impl->context;
}

void* Device::nativeDevice() const
{
    return impl->context.getDevice().Get();
}

void* Device::nativeQueue() const
{
    return impl->context.getQueue().Get();
}

void* Device::nativeTextureCache() const
{
    return nullptr;
}

void* Device::nativeSampler(TextureSampling sampling) const
{
    if (!isValid())
        return nullptr;

    return impl->context.getSampler(sampling).Get();
}

void* Device::createNativeSampler(const SamplerDescriptor& descriptor)
{
    auto sampler = impl->context.makeSampler(descriptor);

    if (sampler == nullptr)
        return nullptr;

    impl->samplers.add(sampler);
    return sampler.Get();
}

bool Device::supportsPerBindSamplers() const
{
    return isValid();
}

void Device::trackSubmittedWork(void*) {}

void Device::waitForSubmittedWork()
{
    impl->context.waitIdle();
}
} // namespace eacp::GPU
