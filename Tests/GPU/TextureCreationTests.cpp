#include "Common.h"

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

auto tTargetReportsWhatItWasAskedFor =
    test("TextureCreation/aTargetReportsWhatItWasAskedFor") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto samples = 4;

    // A refused count gives an invalid texture, so there is nothing to assert.
    if (!device.supportsSampleCount(samples))
        return;

    auto descriptor = TextureDescriptor {};
    descriptor.width = 32;
    descriptor.height = 16;
    descriptor.renderTarget = true;
    descriptor.depth = true;
    descriptor.stencil = true;
    descriptor.sampleableDepth = true;
    descriptor.sampleCount = samples;

    auto target = device.makeTexture(descriptor);

    check(target.isValid());
    check(target.isRenderTarget());
    check(target.sampleCount() == samples);
    check(target.hasDepth());
    check(target.hasStencil());
    check(target.hasSampleableDepth());

    check(target.width() == 32);
    check(target.height() == 16);
    check(target.mipLevels() == 1);
    check(!target.isCube());
    check(!target.isComputeWritable());
};

auto tSingleSampledDepthIsSampleable =
    test("TextureCreation/aSingleSampledDepthIsSampleable") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto descriptor = TextureDescriptor {};
    descriptor.width = 8;
    descriptor.height = 8;
    descriptor.renderTarget = true;
    descriptor.sampleableDepth = true;

    auto target = device.makeTexture(descriptor);

    check(target.isValid());
    check(target.sampleCount() == 1);

    // sampleableDepth implies depth without being asked for.
    check(target.hasDepth());
    check(target.hasSampleableDepth());
    check(!target.hasStencil());
};

auto tPlainTextureIsNoneOfThose =
    test("TextureCreation/aPlainTextureIsNoneOfThose") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    const std::uint32_t pixels[] = {0xff0000ff, 0xff00ff00, 0xffff0000, 0xffffffff};

    auto descriptor = TextureDescriptor {};
    descriptor.width = 2;
    descriptor.height = 2;

    // Ignored rather than refused on a texture that is not a render target.
    descriptor.depth = true;
    descriptor.stencil = true;

    auto texture = device.makeTexture(descriptor, pixels);

    check(texture.isValid());
    check(!texture.isRenderTarget());
    check(!texture.hasDepth());
    check(!texture.hasStencil());
    check(!texture.hasSampleableDepth());
    check(texture.sampleCount() == 1);
    check(texture.mipLevels() == 1);
};

auto tASizeOfNothingIsRefused = test("TextureCreation/aSizeOfNothingIsRefused") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    const auto refused = [&](int width, int height)
    {
        auto descriptor = TextureDescriptor {};
        descriptor.width = width;
        descriptor.height = height;

        return !device.makeTexture(descriptor).isValid();
    };

    check(refused(0, 4));
    check(refused(4, 0));
    check(refused(0, 0));
    check(refused(-4, 4));
};
