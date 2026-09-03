#include "Common.h"

#include <cstdint>

// TextureDescriptor::mipLevels - a chain the *caller* built, uploaded as it
// arrived.
//
// MipmapTests covers the chain eacp builds: the filter, its arithmetic, and the
// fact that a minified draw reaches a level below 0. What none of that can check
// is whether eacp will take a chain it did not build - and there is exactly one
// way to see the difference from outside, which is to hand over levels the
// filter would never have produced. So level 0 here is red and every level below
// it is green, which no averaging of a red image could ever arrive at. A draw at
// full size reads level 0 and a draw at an eighth of it cannot, and neither
// answer is available to a texture whose lower levels were built from its top
// one.
//
// The rest of the file is the refusals, and they are refusals rather than
// precedences on purpose: `mipmapped` and `mipLevels` say opposite things about
// who builds the chain, and a library that picked one of them for you would be
// picking which of two pictures the texture holds. See TextureDescriptor::
// mipLevels for each one.
//
// Runs on both backends, and self-skips without a GPU device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr std::uint32_t red = 0xff0000ff; // RGBA8, little-endian: A B G R
constexpr std::uint32_t green = 0xff00ff00;

// Seven levels: 64, 32, 16, 8, 4, 2, 1.
constexpr auto textureSize = 64;

// An eighth of it, so the sampler is three levels down and choosing rather than
// magnifying - the size MipmapTests minifies to, for the same reason.
constexpr auto minifiedSize = 8;

struct QuadVertex
{
    float position[2];
    float uv[2];
};
} // namespace

EACP_SHADER_VALUE(QuadVertex, Float2)

namespace
{
constexpr QuadVertex unitQuad[] = {
    {{-1.f, -1.f}, {0.f, 1.f}},
    {{1.f, -1.f}, {1.f, 1.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
    {{1.f, -1.f}, {1.f, 1.f}},
    {{1.f, 1.f}, {1.f, 0.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
};

struct ChainShader final : ShaderProgram
{
    ChainShader()
    {
        // Nearest on both filters, so the level the sampler picked comes back as
        // itself rather than blended with the one beside it. Every level here is
        // a flat colour, so this is about the *level* and never about the texel.
        image.sampling = {TextureFilter::Nearest, TextureAddressMode::Clamp};
        compile();
    }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        auto uv = vertexInput(&QuadVertex::uv);

        setPosition(float4(position, 0.f, 1.f));
        setFragment(sample(image, varying(uv)));
    }

    Uniform<Texture2D> image;

    EACP_SHADER(image)
};

struct ChainView final : GPUView
{
    explicit ChainView(Texture& textureToShow)
    {
        setSampleCount(1);

        shader.setVertices(unitQuad, 6);
        shader.image = textureToShow;
        shader.prepare(sampleCount());
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({{0.f, 0.f, 1.f, 1.f}});
        pass.draw(shader);
    }

    ChainShader shader;
};

Graphics::Image drawAt(Texture& texture, int size)
{
    auto view = ChainView {texture};
    view.setBounds({0.f, 0.f, (float) size, (float) size});

    return view.renderToImage(1.f);
}

bool isRed(const Graphics::Color& c)
{
    return c.r > 0.75f && c.g < 0.25f;
}
bool isGreen(const Graphics::Color& c)
{
    return c.g > 0.75f && c.r < 0.25f;
}

// The block TextureDescriptor::mipLevels describes: every level tightly packed,
// level 0 first. Red at the top and green all the way down, which is a chain no
// filter could have produced from that top level - which is the whole of how
// these cases tell a supplied chain from a built one.
Vector<std::uint32_t> redOverGreen(int levels)
{
    const auto bytes =
        mipChainBytes(TextureFormat::RGBA8Unorm, textureSize, textureSize, levels);

    auto pixels = Vector<std::uint32_t> {};
    pixels.resize((int) (bytes / sizeof(std::uint32_t)));

    auto index = 0;

    for (auto level = 0; level < levels; ++level)
    {
        const auto width = mipExtent(textureSize, level);
        const auto height = mipExtent(textureSize, level);

        for (auto texel = 0; texel < width * height; ++texel)
            pixels[index++] = level == 0 ? red : green;
    }

    return pixels;
}

Texture makeSupplied(int levels)
{
    static auto pixels = redOverGreen(mipLevelCount(textureSize, textureSize));

    auto descriptor = TextureDescriptor {};
    descriptor.width = textureSize;
    descriptor.height = textureSize;
    descriptor.format = TextureFormat::RGBA8Unorm;
    descriptor.mipLevels = levels;

    return Device::shared().makeTexture(descriptor, pixels.data());
}
} // namespace

// The layout, as arithmetic, before any of it reaches a device: a chain of N
// levels is the sum of the levels' own sizes, which is what a caller assembling
// one has to size its block with.
auto tChainBytes = test("SuppliedMipChain/theLayoutIsTheSumOfItsLevels") = []
{
    // 64x64 down to 1x1 in texels: 4096 + 1024 + 256 + 64 + 16 + 4 + 1.
    check(mipChainBytes(TextureFormat::RGBA8Unorm, 64, 64, 7) == 5461 * 4);

    // A prefix of the chain is a prefix of the block, which is what makes a
    // partial chain - the levels a .dds file actually carries - meaningful.
    check(mipChainBytes(TextureFormat::RGBA8Unorm, 64, 64, 1) == 4096 * 4);
    check(mipChainBytes(TextureFormat::RGBA8Unorm, 64, 64, 2) == 5120 * 4);

    // And the same block the builder produces, which is what lets the two be
    // read the same way.
    auto source = Vector<std::uint32_t> {};
    source.resize(64 * 64);

    const auto built =
        buildMipChain(source.data(), 64, 64, TextureFormat::RGBA8Unorm);

    check(built.isValid());
    check(built.levelCount() == 7);

    auto total = std::size_t {0};

    for (auto level = 0; level < built.levelCount(); ++level)
        total += (std::size_t) built.levels[level].size();

    check(total == mipChainBytes(TextureFormat::RGBA8Unorm, 64, 64, 7));
};

// The texture reports the count it was given, which is how a caller can tell a
// chain that was taken from one that was refused.
auto tReportsItsLevels = test("SuppliedMipChain/theTextureReportsItsLevels") = []
{
    if (!Device::shared().isValid())
        return;

    auto supplied = makeSupplied(mipLevelCount(textureSize, textureSize));

    check(supplied.isValid());
    check(supplied.mipLevels() == 7);

    // One level is a texture with no chain at all, and is the control the
    // drawing case below leans on.
    auto single = makeSupplied(1);

    check(single.isValid());
    check(single.mipLevels() == 1);
};

// **The case that matters.** The same pixels, the same shader, the same
// sampling; the only difference is whether the levels below 0 were handed over.
//
// With them, a draw at an eighth of the texture's size reads one of them and
// comes back green - a colour that appears nowhere in level 0, so no amount of
// filtering at the top could have produced it. Without them the same draw has
// only level 0 to read and comes back red, which is also what the full-size draw
// gives either way.
auto tMinifiedDrawReadsASuppliedLevel =
    test("SuppliedMipChain/aMinifiedDrawReadsASuppliedLevel") = []
{
    if (!Device::shared().isValid())
        return;

    auto supplied = makeSupplied(mipLevelCount(textureSize, textureSize));
    auto single = makeSupplied(1);

    if (!supplied.isValid() || !single.isValid())
        return;

    const auto minified = drawAt(supplied, minifiedSize);
    const auto full = drawAt(supplied, textureSize);
    const auto control = drawAt(single, minifiedSize);

    if (!minified.isValid() || !full.isValid() || !control.isValid())
        return;

    // Every pixel rather than a sample of them, so the answer does not depend on
    // where the sampler happened to land.
    for (auto y = 0; y < minified.height(); ++y)
        for (auto x = 0; x < minified.width(); ++x)
        {
            check(isGreen(minified.at(x, y)));
            check(isRed(control.at(x, y)));
        }

    check(isRed(full.at(textureSize / 2, textureSize / 2)));
};

// update() on such a texture takes the same block the constructor took - all N
// levels - so a re-upload replaces the whole chain rather than the top of it.
// The levels are inverted here, so a stale lower level shows up as red where the
// new chain says green.
//
// This is also the case that reaches the D3D12 re-upload path, which has to move
// the resource out of PIXEL_SHADER_RESOURCE before it can write to any level.
auto tUpdateReplacesTheWholeChain =
    test("SuppliedMipChain/updateReplacesEveryLevel") = []
{
    if (!Device::shared().isValid())
        return;

    const auto levels = mipLevelCount(textureSize, textureSize);
    auto texture = makeSupplied(levels);

    if (!texture.isValid())
        return;

    // Green at the top and red all the way down: the mirror of what it holds.
    auto inverted = Vector<std::uint32_t> {};
    inverted.resize(
        (int) (mipChainBytes(
                   TextureFormat::RGBA8Unorm, textureSize, textureSize, levels)
               / sizeof(std::uint32_t)));

    auto index = 0;

    for (auto level = 0; level < levels; ++level)
    {
        const auto extent = mipExtent(textureSize, level);

        for (auto texel = 0; texel < extent * extent; ++texel)
            inverted[index++] = level == 0 ? green : red;
    }

    texture.update(inverted.data());

    const auto minified = drawAt(texture, minifiedSize);
    const auto full = drawAt(texture, textureSize);

    if (!minified.isValid() || !full.isValid())
        return;

    check(isGreen(full.at(textureSize / 2, textureSize / 2)));

    for (auto y = 0; y < minified.height(); ++y)
        for (auto x = 0; x < minified.width(); ++x)
            check(isRed(minified.at(x, y)));
};

// A supplied chain is tightly packed by definition, so a row stride is a number
// that can only be wrong. The update below hands one over and nothing moves,
// which is the only way a no-op can be seen from outside.
auto tStrideIsRefused = test("SuppliedMipChain/aStrideIsRefused") = []
{
    if (!Device::shared().isValid())
        return;

    const auto levels = mipLevelCount(textureSize, textureSize);
    auto texture = makeSupplied(levels);

    check(texture.isValid());

    if (!texture.isValid())
        return;

    // A whole chain's worth of green, not just a level 0's worth: this case
    // asserts that the upload does *not* happen, and a buffer sized for one
    // level would be walked off the end rather than left alone if the guard
    // ever regressed - which would crash instead of failing the check below.
    auto flat = Vector<std::uint32_t> {};
    flat.resize(
        (int) (mipChainBytes(
                   TextureFormat::RGBA8Unorm, textureSize, textureSize, levels)
               / sizeof(std::uint32_t)));

    for (auto i = 0; i < flat.size(); ++i)
        flat[i] = green;

    texture.update(flat.data(), textureSize * sizeof(std::uint32_t));

    const auto full = drawAt(texture, textureSize);

    if (!full.isValid())
        return;

    check(isRed(full.at(textureSize / 2, textureSize / 2)));
};

// The refusals, each of which is a texture that does not exist rather than one
// that quietly means something else.
//
// `mipmapped` beside `mipLevels` is the one worth spelling out: the two say
// opposite things about who builds the chain, and there is no reading of them
// together that is not one of them being ignored.
auto tRefusals = test("SuppliedMipChain/refusesWhatItCannotMean") = []
{
    if (!Device::shared().isValid())
        return;

    static auto pixels = redOverGreen(mipLevelCount(textureSize, textureSize));

    auto base = TextureDescriptor {};
    base.width = textureSize;
    base.height = textureSize;
    base.format = TextureFormat::RGBA8Unorm;
    base.mipLevels = mipLevelCount(textureSize, textureSize);

    {
        auto both = base;
        both.mipmapped = true;

        check(!Device::shared().makeTexture(both, pixels.data()).isValid());
    }

    {
        // One level more than the size has, which is a level with no extent.
        auto tooMany = base;
        tooMany.mipLevels = mipLevelCount(textureSize, textureSize) + 1;

        check(!Device::shared().makeTexture(tooMany, pixels.data()).isValid());
    }

    {
        auto negative = base;
        negative.mipLevels = -1;

        check(!Device::shared().makeTexture(negative, pixels.data()).isValid());
    }

    {
        // **Null pixels, which is the one that would otherwise have been
        // silent.** Nothing is uploaded when there are no pixels, so a texture
        // created here would carry N levels the sampler happily reads and
        // nothing ever wrote - the exact failure `mipmapped` avoids by asking
        // for pixels before it builds a chain. Refused instead.
        check(!Device::shared().makeTexture(base, nullptr).isValid());
    }

    {
        // A render target's pixels come from the GPU, so there was never a chain
        // to supply - and a kernel output is the same case.
        auto target = base;
        target.renderTarget = true;

        check(!Device::shared().makeTexture(target, nullptr).isValid());

        auto kernelOutput = base;
        kernelOutput.computeWrite = true;

        check(!Device::shared().makeTexture(kernelOutput, nullptr).isValid());
    }

    {
        auto cube = base;
        cube.cube = true;
        cube.width = 4;
        cube.height = 4;
        cube.mipLevels = 3;

        check(!Device::shared().makeTexture(cube, pixels.data()).isValid());
    }

    // And the shape that is *not* refused, so the cases above are refusing what
    // they name rather than everything.
    check(Device::shared().makeTexture(base, pixels.data()).isValid());
};

// A partial chain: fewer levels than the size has, which is what a .dds file
// that stops above 1x1 carries. The texture takes the count it was given and the
// sampler never reaches below it.
auto tPartialChain = test("SuppliedMipChain/aPartialChainIsTakenAsItIs") = []
{
    if (!Device::shared().isValid())
        return;

    auto texture = makeSupplied(2);

    check(texture.isValid());
    check(texture.mipLevels() == 2);

    if (!texture.isValid())
        return;

    // Level 1 is green, and an eighth-size draw clamps to the lowest level the
    // texture has rather than reading one that is not there.
    const auto minified = drawAt(texture, minifiedSize);

    if (!minified.isValid())
        return;

    for (auto y = 0; y < minified.height(); ++y)
        for (auto x = 0; x < minified.width(); ++x)
            check(isGreen(minified.at(x, y)));
};
