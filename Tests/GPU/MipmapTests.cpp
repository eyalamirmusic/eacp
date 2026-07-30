#include "Common.h"

#include <cstdint>

// TextureDescriptor::mipmapped - the chain, the filter that builds it, and the
// sampler that reads it.
//
// The chain itself is arithmetic and is checked as arithmetic: a known 2x2
// block has a known average, and that assertion needs no GPU and cannot flake.
// What it cannot check is whether the levels reach the hardware and whether a
// minified draw actually samples one, which is a different question on each
// backend - Metal fills the levels through replaceRegion:mipmapLevel: and D3D12
// through a copy per subresource - and is what the drawing case is for.
//
// The sampler side had a real bug in it before this: D3D12's static samplers
// have always declared MIN_MAG_MIP_LINEAR and MIN_MAG_MIP_POINT, while Metal
// left mipFilter at its default of NotMipmapped. Nothing could see it, because
// no texture had a second level. The first one would have been filtered across
// levels on Windows and read at full size on Apple - so the drawing case below
// is the one that would have caught it, and now holds the two together.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr std::uint32_t red = 0xff0000ff; // RGBA8, little-endian: A B G R
constexpr std::uint32_t green = 0xff00ff00;

// Big enough that a minified draw is well down the chain, and a power of two so
// every level halves exactly.
constexpr auto textureSize = 64;

// The drawn size, so the texture is minified 8x and the sampler is choosing a
// level rather than magnifying level 0.
constexpr auto viewSize = 8;

struct QuadVertex
{
    float position[2];
    float uv[2];
};
} // namespace

EACP_SHADER_VALUE(QuadVertex, Float2)

namespace
{
// The whole texture across the whole viewport.
constexpr QuadVertex unitQuad[] = {
    {{-1.f, -1.f}, {0.f, 1.f}},
    {{1.f, -1.f}, {1.f, 1.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
    {{1.f, -1.f}, {1.f, 1.f}},
    {{1.f, 1.f}, {1.f, 0.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
};

struct MipShader final : ShaderProgram
{
    MipShader()
    {
        // Nearest, and that is what makes the drawing case decisive. Linear
        // would average a 2x2 neighbourhood of level 0, and a 2x2 neighbourhood
        // of a checkerboard is two red texels and two green ones - the same
        // blend a mip level holds. The case would then pass with no mips at all.
        // Nearest takes exactly one texel, so without a chain the answer is a
        // pure colour and with one it cannot be.
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

struct MipView final : GPUView
{
    explicit MipView(Texture& textureToShow)
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

    MipShader shader;
};

// A one-texel checkerboard of red and green.
//
// Chosen because its average is a colour no texel of it holds: every level
// below 0 is a uniform half-red half-green, while level 0 is pure red or pure
// green everywhere. So "did this draw sample a mip level" has a yes/no answer
// that does not depend on *which* texel or *which* level the hardware picked -
// which is what keeps the case from turning into an argument about filtering
// rules that differ between GPUs.
Vector<std::uint32_t> checkerboard()
{
    auto pixels = Vector<std::uint32_t> {};
    pixels.resize(textureSize * textureSize);

    for (auto y = 0; y < textureSize; ++y)
        for (auto x = 0; x < textureSize; ++x)
            pixels[y * textureSize + x] = ((x + y) % 2) == 0 ? red : green;

    return pixels;
}

Texture makeCheckerboard(bool mipmapped)
{
    static auto pixels = checkerboard();

    auto descriptor = TextureDescriptor {};
    descriptor.width = textureSize;
    descriptor.height = textureSize;
    descriptor.format = TextureFormat::RGBA8Unorm;
    descriptor.mipmapped = mipmapped;

    return Device::shared().makeTexture(descriptor, pixels.data());
}

Graphics::Image drawMinified(Texture& texture)
{
    auto view = MipView {texture};
    view.setBounds({0.f, 0.f, (float) viewSize, (float) viewSize});

    return view.renderToImage(1.f);
}

// Whether a colour is a mix of the two rather than one of them. A texel of the
// checkerboard fails this; any level below the top passes it.
bool isBlended(const Graphics::Color& c)
{
    const auto smaller = c.r < c.g ? c.r : c.g;
    return smaller > 0.25f;
}
} // namespace

// The chain's shape, which is arithmetic and needs no device: it halves to 1x1
// and keeps halving the longer axis after the shorter one has bottomed out,
// which is what both APIs assume when they index a subresource.
auto tChainReachesOneByOne = test("Mipmap/chainReachesOneByOne") = []
{
    check(mipLevelCount(64, 64) == 7);
    check(mipLevelCount(64, 16) == 7); // 64x16 down to 1x1 is still seven
    check(mipLevelCount(1, 1) == 1);
    check(mipLevelCount(3, 1) == 2);

    // Never below one, which is what an odd extent's last halving relies on.
    check(mipExtent(64, 3) == 8);
    check(mipExtent(1, 4) == 1);
    check(mipExtent(3, 1) == 1);
};

// The filter itself, against numbers chosen so the answer can be written down.
// Four texels, two black and two white, average to exactly half - and 128 is
// what rounding to nearest gives, where truncating would give 127 and lose a
// little brightness at every level of a long chain.
auto tLevelsAverageTheOneAbove = test("Mipmap/levelsAverageTheOneAbove") = []
{
    // clang-format off
    const std::uint8_t pixels[] = {
          0,   0,   0, 255,     0,   0,   0, 255,  // two black texels
        255, 255, 255, 255,   255, 255, 255, 255,  // and two white ones
    };
    // clang-format on

    const auto chain = buildMipChain(pixels, 2, 2, TextureFormat::RGBA8Unorm);

    check(chain.isValid());
    check(chain.levelCount() == 2);

    const auto* level1 = static_cast<const std::uint8_t*>(chain.level(1));

    check(level1[0] == 128);
    check(level1[1] == 128);
    check(level1[2] == 128);
    check(level1[3] == 255); // alpha was 255 everywhere and stays there
};

// A texture with no pixels has nothing to build a chain from, so it does not get
// one. Worth stating because the alternative is worse than no mips: levels the
// sampler will happily read and nothing ever wrote.
auto tNoPixelsMeansNoChain = test("Mipmap/noPixelsMeansNoChain") = []
{
    if (!Device::shared().isValid())
        return;

    auto descriptor = TextureDescriptor {};
    descriptor.width = 64;
    descriptor.height = 64;
    descriptor.format = TextureFormat::RGBA8Unorm;
    descriptor.renderTarget = true;
    descriptor.mipmapped = true;

    auto target = Device::shared().makeTexture(descriptor, nullptr);

    check(target.isValid());
    check(target.mipLevels() == 1);
};

// The texture reports what it got, which is how a caller can tell a chain it
// asked for from one the format or the missing pixels refused.
auto tMipmappedTextureReportsItsLevels = test("Mipmap/textureReportsItsLevels") = []
{
    if (!Device::shared().isValid())
        return;

    auto plain = makeCheckerboard(false);
    auto mipmapped = makeCheckerboard(true);

    check(plain.isValid());
    check(mipmapped.isValid());

    check(plain.mipLevels() == 1);
    check(mipmapped.mipLevels() == mipLevelCount(textureSize, textureSize));
};

// update() on a mipmapped texture has to rebuild every level, not just the top
// one - otherwise a re-uploaded texture keeps showing its previous contents at
// any distance, which is the kind of bug that only appears once the camera pulls
// back. The checkerboard is replaced with flat red: with the chain rebuilt every
// level is red, and a minified draw is red. Left stale, the lower levels still
// hold the old blend and the draw comes back blended.
//
// This is also the only case that reaches the D3D12 re-upload path, which has to
// move the resource back out of PIXEL_SHADER_RESOURCE before it can write to it
// - a step the create-time upload does not need.
auto tUpdateRebuildsTheChain = test("Mipmap/updateRebuildsTheChain") = []
{
    if (!Device::shared().isValid())
        return;

    auto texture = makeCheckerboard(true);

    if (!texture.isValid() || texture.mipLevels() <= 1)
        return;

    auto flat = Vector<std::uint32_t> {};
    flat.resize(textureSize * textureSize);

    for (auto i = 0; i < flat.size(); ++i)
        flat[i] = red;

    texture.update(flat.data());

    const auto image = drawMinified(texture);

    if (!image.isValid())
        return;

    for (auto y = 0; y < image.height(); ++y)
        for (auto x = 0; x < image.width(); ++x)
        {
            const auto pixel = image.at(x, y);

            check(pixel.r > 0.75f);
            check(pixel.g < 0.25f); // no trace of the checkerboard's green
        }
};

// The case that matters, and the only one that touches the hardware: the same
// checkerboard, drawn at an eighth of its size, through the same shader and the
// same Nearest sampling. The one difference is whether the texture has a chain.
//
// Without one there is only level 0, so every pixel is a single checkerboard
// texel - pure red or pure green. With one, the sampler drops to a level whose
// every texel is the average of the two, and no pixel can be pure anything.
//
// This is also the case that pins the two backends together on mip filtering,
// which they disagreed about until this was written.
auto tMinifiedDrawSamplesABlendedLevel =
    test("Mipmap/minifiedDrawSamplesABlendedLevel") = []
{
    if (!Device::shared().isValid())
        return;

    auto plain = makeCheckerboard(false);
    auto mipmapped = makeCheckerboard(true);

    const auto withoutMips = drawMinified(plain);
    const auto withMips = drawMinified(mipmapped);

    if (!withoutMips.isValid() || !withMips.isValid())
        return;

    // Every pixel, not a sample of them: with a chain none can be a pure texel,
    // and without one none can be a blend. A single-pixel check would be at the
    // mercy of where the sampler happened to land.
    for (auto y = 0; y < withMips.height(); ++y)
        for (auto x = 0; x < withMips.width(); ++x)
        {
            check(isBlended(withMips.at(x, y)));
            check(!isBlended(withoutMips.at(x, y)));
        }
};
