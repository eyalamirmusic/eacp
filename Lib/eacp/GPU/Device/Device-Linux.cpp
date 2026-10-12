#include "Device.h"

#include "../Vulkan/VulkanContext.h"
#include "../Vulkan/VulkanTypes.h"

#include <algorithm>

namespace eacp::GPU
{
namespace
{
VkSamplerAddressMode toVkAddressMode(SamplerAddressMode mode)
{
    switch (mode)
    {
        case SamplerAddressMode::ClampToEdge:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case SamplerAddressMode::Repeat:
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case SamplerAddressMode::MirrorRepeat:
            return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case SamplerAddressMode::ClampToBorder:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }

    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
}

VkBorderColor toVkBorderColor(SamplerBorderColor color)
{
    switch (color)
    {
        case SamplerBorderColor::TransparentBlack:
            return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        case SamplerBorderColor::OpaqueBlack:
            return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        case SamplerBorderColor::OpaqueWhite:
            return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    }

    return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
}

VkFilter toVkFilter(SamplerFilter filter)
{
    return filter == SamplerFilter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
}

// Vulkan has no "not mipmapped" mode; the spec's own recipe is a nearest mip
// mode with maxLod 0.25, which keeps level 0 while still telling minification
// from magnification.
VkSamplerCreateInfo toVkSamplerInfo(const SamplerDescriptor& descriptor)
{
    const auto& shared = getVulkanShared();
    const auto mipmapped = descriptor.mipFilter != SamplerMipFilter::None;
    const auto anisotropic = descriptor.maxAnisotropy > 1
                             && descriptor.isFullyLinear()
                             && shared.supportsSamplerAnisotropy();

    VkSamplerCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = toVkFilter(descriptor.magFilter);
    info.minFilter = toVkFilter(descriptor.minFilter);
    info.mipmapMode = descriptor.mipFilter == SamplerMipFilter::Linear
                          ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                          : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = toVkAddressMode(descriptor.addressU);
    info.addressModeV = toVkAddressMode(descriptor.addressV);
    info.addressModeW = toVkAddressMode(descriptor.addressW);
    info.anisotropyEnable = anisotropic ? VK_TRUE : VK_FALSE;
    info.maxAnisotropy =
        anisotropic ? std::min((float) descriptor.maxAnisotropy,
                               shared.getProperties().limits.maxSamplerAnisotropy)
                    : 1.f;
    info.compareEnable = descriptor.compare ? VK_TRUE : VK_FALSE;
    info.compareOp = descriptor.compare ? toVkCompareOp(*descriptor.compare)
                                        : VK_COMPARE_OP_NEVER;
    info.minLod = mipmapped ? descriptor.lodMinClamp : 0.f;
    info.maxLod =
        mipmapped ? std::min(descriptor.lodMaxClamp, VK_LOD_CLAMP_NONE) : 0.25f;
    info.borderColor = toVkBorderColor(descriptor.borderColor);
    return info;
}

// The samplers makeSampler created beyond the four VulkanShared owns. Declared
// before the context in Native so they are destroyed after it has waited for
// the GPU to go idle.
struct OwnedSamplers
{
    ~OwnedSamplers()
    {
        const auto device = getVulkanShared().getDevice();

        if (device == VK_NULL_HANDLE)
            return;

        for (auto sampler: samplers)
            vkDestroySampler(device, sampler, nullptr);
    }

    Vector<VkSampler> samplers;
};
} // namespace

struct Device::Native
{
    OwnedSamplers samplers;

    // Mutable because the accessors that reach it are const, and a const Device
    // still submits.
    mutable VulkanContext context;
};

Device::Device()
    : impl()
{
}

Device& Device::shared()
{
    static Device instance;

    // Belongs to the main thread whichever thread asked for it first.
    [[maybe_unused]] static const auto boundToMainThread =
        (instance.followMainThread(),
         instance.impl->context.followMainThread(),
         true);

    return instance;
}

VulkanContext& getVulkanContext(const Device& device)
{
    return *static_cast<VulkanContext*>(device.nativeContext());
}

bool Device::isValid() const
{
    return impl->context.isValid();
}

std::string Device::name() const
{
    if (!isValid())
        return "no Vulkan device";

    return getVulkanShared().getAdapterName();
}

// The intersection of the three masks a pass touches.
bool Device::supportsSampleCount(int count) const
{
    if (count <= 1)
        return true;

    if (!isValid() || count > 64 || (count & (count - 1)) != 0)
        return false;

    const auto& limits = getVulkanShared().getProperties().limits;
    const auto bit = static_cast<VkSampleCountFlags>(count);

    return (limits.framebufferColorSampleCounts & bit) != 0
           && (limits.framebufferDepthSampleCounts & bit) != 0
           && (limits.sampledImageColorSampleCounts & bit) != 0;
}

// BC support is optional in Vulkan.
bool Device::supportsBlockCompression() const
{
    return isValid()
           && getVulkanShared().getFeatures().textureCompressionBC == VK_TRUE;
}

// The one range rule that is a device property rather than a constant: a
// descriptor may name no offset off minStorageBufferOffsetAlignment.
int Device::storageBufferOffsetAlignment() const
{
    if (!isValid())
        return 4;

    const auto alignment =
        getVulkanShared().getProperties().limits.minStorageBufferOffsetAlignment;

    return alignment > 0 ? (int) alignment : 4;
}

bool Device::supportsIndirectCount() const
{
    return isValid() && getVulkanShared().supportsIndirectCount();
}

// Occlusion query pools are core; an exact count needs occlusionQueryPrecise.
bool Device::supportsOcclusionQueries() const
{
    return isValid();
}

bool Device::supportsOcclusionSampleCounts() const
{
    return isValid() && getVulkanShared().supportsPreciseOcclusionQueries();
}

int Device::maxTextureSlots() const
{
    return isValid() ? vulkanTextureSlots() : 0;
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
    return isValid() ? vulkanComputeBufferSlots() : 0;
}

int Device::maxTextureLayers() const
{
    if (!isValid())
        return 0;

    return (int) getVulkanShared().getProperties().limits.maxImageArrayLayers;
}

int Device::maxTextureSize3D() const
{
    if (!isValid())
        return 0;

    return (int) getVulkanShared().getProperties().limits.maxImageDimension3D;
}

bool Device::supportsCubeArrays() const
{
    return isValid() && getVulkanShared().getFeatures().imageCubeArray == VK_TRUE;
}

bool Device::supportsLayeredRendering() const
{
    return isValid() && getVulkanShared().supportsLayeredRendering();
}

// One answer for both: an image declared with the format's qualifier loads
// wherever it stores. A shader that declares none - the emitter's writeonly
// image2D - leans on shaderStorageImageWriteWithoutFormat instead.
bool Device::supportsComputeWrite(TextureFormat format) const
{
    if (!isValid() || !isRenderableFormat(format) || isSrgbFormat(format)
        || format == TextureFormat::BGRA8Unorm)
        return false;

    VkFormatProperties properties = {};
    vkGetPhysicalDeviceFormatProperties(
        getVulkanShared().getPhysicalDevice(), toVkFormat(format), &properties);

    return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)
           != 0;
}

bool Device::supportsComputeReadWrite(TextureFormat format) const
{
    return supportsComputeWrite(format);
}

// A chain of vkCmdBlitImage with a linear filter, level to level.
bool Device::supportsMipGeneration(TextureFormat format) const
{
    // A blit into E5B9G9R9 is not something Vulkan promises (BLIT_DST rides
    // on the colour-attachment feature), so it is refused without asking.
    if (!isValid() || !isRenderableFormat(format) || isIntegerFormat(format))
        return false;

    VkFormatProperties properties = {};
    vkGetPhysicalDeviceFormatProperties(
        getVulkanShared().getPhysicalDevice(), toVkFormat(format), &properties);

    constexpr auto needed = VkFormatFeatureFlags {
        VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT
        | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT};

    return (properties.optimalTilingFeatures & needed) == needed;
}

int Device::maxThreadgroupMemory() const
{
    if (!isValid())
        return 0;

    return (int) getVulkanShared().getProperties().limits.maxComputeSharedMemorySize;
}

// No, and not because of the hardware: eacp emits GLSL 450 with no
// cooperative-matrix extension, so a fragment here is the two-floats-per-lane
// emulation whatever the driver could have done. The packed loads work on it -
// each lane unpacks the pair it holds - and are simply not faster, so there is
// nothing for a kernel to restructure itself around.
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
    return getVulkanShared().getDevice();
}

void* Device::nativeQueue() const
{
    return getVulkanShared().getQueue();
}

void* Device::nativeTextureCache() const
{
    // No zero-copy pixel-buffer path on Linux.
    return nullptr;
}

// For callers outside the backend: the bind sites take the sampler straight off
// VulkanShared.
void* Device::nativeSampler(TextureSampling sampling) const
{
    if (!isValid())
        return nullptr;

    return getVulkanShared().getSampler(sampling);
}

void* Device::createNativeSampler(const SamplerDescriptor& descriptor)
{
    const auto info = toVkSamplerInfo(descriptor);
    auto sampler = VkSampler {VK_NULL_HANDLE};

    if (vkCreateSampler(getVulkanShared().getDevice(), &info, nullptr, &sampler)
        != VK_SUCCESS)
        return nullptr;

    impl->samplers.samplers.add(sampler);
    return reinterpret_cast<void*>(sampler);
}

bool Device::supportsPerBindSamplers() const
{
    return isValid();
}

void Device::trackSubmittedWork(void*)
{
    // Nothing to record: every submit already signals this Device's timeline.
}

void Device::waitForSubmittedWork()
{
    impl->context.waitIdle();
}
} // namespace eacp::GPU
