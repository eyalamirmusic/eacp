#include "Common.h"

#include <cstdint>

// Regression: D3D12's static samplers declared MIN_MAG_MIP_LINEAR while Metal
// left mipFilter at NotMipmapped, invisible until a texture had a second level.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr std::uint32_t red = 0xff0000ff; // RGBA8, little-endian: A B G R
constexpr std::uint32_t green = 0xff00ff00;

// A power of two, so every level halves exactly.
constexpr auto textureSize = 64;

// An eighth of the texture, so the sampler chooses a level rather than
// magnifying level 0.
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
        // Nearest is what makes the drawing case decisive: linear would average
        // a 2x2 block of level 0, which on a checkerboard is the same blend a
        // mip level holds, and the case would pass with no mips at all.
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

// Its average is a colour no texel of it holds, so "did this sample a mip
// level" answers yes or no whichever texel or level the hardware picked.
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

// A checkerboard texel fails this; any level below the top passes it.
bool isBlended(const Graphics::Color& c)
{
    const auto smaller = c.r < c.g ? c.r : c.g;
    return smaller > 0.25f;
}
} // namespace

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

// Two black texels and two white average to exactly half: 128 is round-to-
// nearest, where truncating would give 127 and darken every level of a chain.
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

// Left stale, the lower levels still hold the old blend. Also the only case
// reaching D3D12's re-upload path, which has to move the resource back out of
// PIXEL_SHADER_RESOURCE first.
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

// The only case that touches the hardware, and what pins the two backends
// together on mip filtering.
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

    // Every pixel: a single-pixel check would depend on where the sampler landed.
    for (auto y = 0; y < withMips.height(); ++y)
        for (auto x = 0; x < withMips.width(); ++x)
        {
            check(isBlended(withMips.at(x, y)));
            check(!isBlended(withoutMips.at(x, y)));
        }
};
