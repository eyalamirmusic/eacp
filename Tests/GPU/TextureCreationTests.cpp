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

// A texture created with no pixels is never written before it is read, which on
// a backend that tracks image layouts means it is read out of the layout it was
// created in. The contents are undefined; that the read runs at all, and that
// the texture still takes an upload afterwards, is what is pinned.
auto tAnUnwrittenTextureCanBeRead =
    test("TextureCreation/anUnwrittenTextureCanBeRead") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto size = 4;

    auto descriptor = TextureDescriptor {};
    descriptor.width = size;
    descriptor.height = size;

    auto texture = device.makeTexture(descriptor);

    check(texture.isValid());

    auto read = Array<unsigned char, size * size * 4> {};
    texture.read(read.data());

    auto pixels = Array<unsigned char, size * size * 4> {};

    for (auto i = 0; i < pixels.size(); ++i)
        pixels[i] = (unsigned char) (i * 3 + 1);

    texture.update(pixels.data());
    texture.read(read.data());

    check(read == pixels);
};

// A stride is a byte count, so a negative one is a call that cannot be honoured
// rather than one to reinterpret as an enormous unsigned pitch.
auto tANegativeStrideIsRefused =
    test("TextureCreation/aNegativeStrideIsRefused") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto size = 4;

    auto pixels = Array<unsigned char, size * size * 4> {};
    pixels.fill(0x7f);

    auto descriptor = TextureDescriptor {};
    descriptor.width = size;
    descriptor.height = size;

    auto texture = device.makeTexture(descriptor, pixels.data());

    check(texture.isValid());

    auto other = Array<unsigned char, size * size * 4> {};
    other.fill(0x11);

    texture.update(other.data(), -(size * 4));
    texture.update({0.f, 0.f, 2.f, 2.f}, other.data(), -(size * 4));

    auto read = Array<unsigned char, size * size * 4> {};
    read.fill(0xab);

    texture.read(read.data(), -(size * 4));

    // Neither the texture nor the destination was touched.
    check(read[0] == 0xab);

    texture.read(read.data());

    check(read == pixels);
};
