#include "Sampler.h"

#include <functional>

namespace eacp::GPU
{
SamplerDescriptor SamplerDescriptor::from(const TextureSampling& sampling)
{
    const auto linear = sampling.filter == TextureFilter::Linear;
    const auto address = sampling.addressMode == TextureAddressMode::Repeat
                             ? SamplerAddressMode::Repeat
                             : SamplerAddressMode::ClampToEdge;

    auto descriptor = SamplerDescriptor {};
    descriptor.addressU = address;
    descriptor.addressV = address;
    descriptor.addressW = address;
    descriptor.minFilter = linear ? SamplerFilter::Linear : SamplerFilter::Nearest;
    descriptor.magFilter = descriptor.minFilter;
    descriptor.mipFilter =
        linear ? SamplerMipFilter::Linear : SamplerMipFilter::Nearest;
    return descriptor;
}

bool SamplerDescriptor::isFullyLinear() const
{
    return minFilter == SamplerFilter::Linear && magFilter == SamplerFilter::Linear
           && mipFilter == SamplerMipFilter::Linear;
}

std::size_t hashOf(const SamplerDescriptor& descriptor)
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

std::size_t
    SamplerDescriptorHash::operator()(const SamplerDescriptor& descriptor) const
{
    return hashOf(descriptor);
}

Sampler::Sampler(void* nativeHandle, const SamplerDescriptor& samplerDescriptor)
    : native(nativeHandle)
    , description(samplerDescriptor)
{
}

bool Sampler::isValid() const
{
    return native != nullptr;
}

const SamplerDescriptor& Sampler::descriptor() const
{
    return description;
}

void* Sampler::nativeSampler() const
{
    return native;
}

bool Sampler::operator==(const Sampler& other) const
{
    return native == other.native;
}
} // namespace eacp::GPU
