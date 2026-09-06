#include "Common.h"

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

// What a descriptor actually gets you, asked of the texture itself rather than
// of a picture. Nothing here draws, which is the point: every one of these
// answers is what a *pass* is then built against - a pipeline's sampleCount has
// to match the target's, a depth-tested pipeline needs hasDepth, a stencil one
// needs hasStencil, and setFragmentDepthTexture needs hasSampleableDepth - so a
// backend that creates the companions but reports them wrong produces a pass
// that fails for reasons a drawing test attributes to the drawing.
//
// It is also the only coverage the multisample and depth companions have on a
// backend whose render half has not landed yet, where every test that draws
// through them self-skips.

// The full shape: a multisampled target that carries depth, a stencil plane and
// a depth buffer something else is going to sample. All four are asked for at
// once because they interact - the depth companion is created at the colour
// target's sample count, and a sampleable depth on a multisampled target is a
// second, single-sampled buffer that the first resolves into.
auto tTargetReportsWhatItWasAskedFor =
    test("TextureCreation/aTargetReportsWhatItWasAskedFor") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto samples = 4;

    // A count the device refuses is an invalid texture by contract, so there is
    // nothing to assert about a target that could not exist. Both other counts
    // this file uses - one - are always available.
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

    // The resolved picture is the texture's own size, not the multisample
    // companion's anything - a target's dimensions do not move with its count.
    check(target.width() == 32);
    check(target.height() == 16);

    // A render target has no chain: its pixels come from the GPU, so there were
    // never any levels to build one from.
    check(target.mipLevels() == 1);
    check(!target.isCube());
    check(!target.isComputeWritable());
};

// The single-sampled half of the same question, which is a different code path
// on every backend: the depth buffer a shader samples *is* the attachment here,
// where the multisampled one above needs a resolve to read.
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

    // sampleableDepth implies depth without being asked, which is what the
    // header says and what a pass attaching one relies on.
    check(target.hasDepth());
    check(target.hasSampleableDepth());
    check(!target.hasStencil());
};

// The negative half, so the answers above mean something. An ordinary sampled
// texture is none of these things, and asking it is how a bind tells a target
// apart from a picture.
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

    // Set and ignored: depth and stencil describe a pass, and there is no pass
    // into a texture that is not a render target. Ignored rather than refused,
    // which is what TextureDescriptor::depth says.
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

// A texture of no size is not a texture. Refused on every backend, and worth
// pinning because the shape a caller reaches it with - a window that has not
// been laid out yet, an image that failed to decode - is one where the zero
// arrives from somewhere else entirely.
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
