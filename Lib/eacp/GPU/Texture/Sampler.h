#pragma once

#include "../Pipeline/RenderPipeline.h"
#include "Texture.h"

#include <cstddef>
#include <functional>
#include <limits>
#include <optional>

namespace eacp::GPU
{
enum class SamplerAddressMode
{
    ClampToEdge,
    Repeat,
    MirrorRepeat,
    ClampToBorder
};

enum class SamplerFilter
{
    Nearest,
    Linear
};

// None reads level 0 whatever other levels exist.
enum class SamplerMipFilter
{
    None,
    Nearest,
    Linear
};

// The three every backend can express without an extension.
enum class SamplerBorderColor
{
    TransparentBlack,
    OpaqueBlack,
    OpaqueWhite
};

// How a sampler reads a texture, described per bind rather than declared by the
// shader. A plain value: equal descriptors are the same sampler, which is what
// lets Device::makeSampler hand back one native object for each.
//
// compare makes it a comparison sampler, for a depth texture sampled through
// sample_compare / SampleCmp / sampler2DShadow: the result is 1 where
// `reference compare texel` holds and 0 where it does not.
//
// maxAnisotropy above 1 takes effect where min, mag and mip filters are all
// Linear, and is clamped to what the device allows.
struct SamplerDescriptor
{
    SamplerAddressMode addressU = SamplerAddressMode::ClampToEdge;
    SamplerAddressMode addressV = SamplerAddressMode::ClampToEdge;
    SamplerAddressMode addressW = SamplerAddressMode::ClampToEdge;

    SamplerFilter minFilter = SamplerFilter::Nearest;
    SamplerFilter magFilter = SamplerFilter::Nearest;
    SamplerMipFilter mipFilter = SamplerMipFilter::None;

    int maxAnisotropy = 1;

    std::optional<CompareFunction> compare;

    SamplerBorderColor borderColor = SamplerBorderColor::TransparentBlack;

    float lodMinClamp = 0.f;
    float lodMaxClamp = std::numeric_limits<float>::max();

    bool operator==(const SamplerDescriptor&) const = default;

    // The descriptor a TextureSampling stands for: one filter for min, mag and
    // mip, one address mode on every axis.
    static constexpr SamplerDescriptor from(const TextureSampling& sampling)
    {
        const auto linear = sampling.filter == TextureFilter::Linear;
        const auto address = sampling.addressMode == TextureAddressMode::Repeat
                                 ? SamplerAddressMode::Repeat
                                 : SamplerAddressMode::ClampToEdge;

        auto descriptor = SamplerDescriptor {};
        descriptor.addressU = address;
        descriptor.addressV = address;
        descriptor.addressW = address;
        descriptor.minFilter =
            linear ? SamplerFilter::Linear : SamplerFilter::Nearest;
        descriptor.magFilter = descriptor.minFilter;
        descriptor.mipFilter =
            linear ? SamplerMipFilter::Linear : SamplerMipFilter::Nearest;
        return descriptor;
    }

    // Whether all three filters are Linear, which is what anisotropy needs.
    constexpr bool isFullyLinear() const
    {
        return minFilter == SamplerFilter::Linear
               && magFilter == SamplerFilter::Linear
               && mipFilter == SamplerMipFilter::Linear;
    }
};

inline std::size_t hashOf(const SamplerDescriptor& descriptor)
{
    auto seed = std::size_t {0};

    const auto combine = [&seed](auto value)
    {
        seed ^= std::hash<decltype(value)> {}(value) + 0x9e3779b97f4a7c15ull
                + (seed << 6) + (seed >> 2);
    };

    combine((int) descriptor.addressU);
    combine((int) descriptor.addressV);
    combine((int) descriptor.addressW);
    combine((int) descriptor.minFilter);
    combine((int) descriptor.magFilter);
    combine((int) descriptor.mipFilter);
    combine(descriptor.maxAnisotropy);
    combine(descriptor.compare.has_value() ? (int) *descriptor.compare : -1);
    combine((int) descriptor.borderColor);
    combine(descriptor.lodMinClamp);
    combine(descriptor.lodMaxClamp);

    return seed;
}

struct SamplerDescriptorHash
{
    std::size_t operator()(const SamplerDescriptor& descriptor) const
    {
        return hashOf(descriptor);
    }
};

// A handle to a sampler the Device made and keeps: copying one is free, and it
// stays valid for as long as that Device lives. Made by Device::makeSampler and
// bound through RenderPass::setFragmentTexture or ComputePass::setInputTexture
// on a pass of the same Device.
//
// nativeSampler() is an id<MTLSamplerState> on Metal and a VkSampler on Vulkan;
// on D3D12 it points at the backend's record of a sampler-heap descriptor.
class Sampler
{
public:
    Sampler() = default;

    bool isValid() const { return native != nullptr; }

    const SamplerDescriptor& descriptor() const { return description; }

    void* nativeSampler() const { return native; }

    bool operator==(const Sampler& other) const { return native == other.native; }

private:
    friend class Device;

    Sampler(void* nativeHandle, const SamplerDescriptor& samplerDescriptor)
        : native(nativeHandle)
        , description(samplerDescriptor)
    {
    }

    void* native = nullptr;
    SamplerDescriptor description;
};
} // namespace eacp::GPU

template <>
struct std::hash<eacp::GPU::SamplerDescriptor> : eacp::GPU::SamplerDescriptorHash
{
};
