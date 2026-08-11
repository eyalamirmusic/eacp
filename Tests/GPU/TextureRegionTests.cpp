#include "Common.h"

// There is no texture read-back API, so these draw the texture 1:1 and read the
// image back. Positional assertions use the x axis only, where neither backend
// mirrors; everything else is asserted by counting texels.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto textureSize = 8;

constexpr std::uint32_t black = 0xff000000;
constexpr std::uint32_t red = 0xff0000ff; // RGBA8, little-endian: A B G R
constexpr std::uint32_t green = 0xff00ff00;

struct QuadVertex
{
    float position[2];
    float uv[2];
};
} // namespace

EACP_SHADER_VALUE(QuadVertex, Float2)

namespace
{
constexpr QuadVertex fullScreenQuad[] = {
    {{-1.f, -1.f}, {0.f, 1.f}},
    {{1.f, -1.f}, {1.f, 1.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
    {{1.f, -1.f}, {1.f, 1.f}},
    {{1.f, 1.f}, {1.f, 0.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
};

struct SampleShader final : ShaderProgram
{
    SampleShader() { compile(); }

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

struct TextureView final : GPUView
{
    explicit TextureView(Texture& textureToShow)
    {
        setSampleCount(1);

        shader.setVertices(fullScreenQuad);
        shader.image = textureToShow;
        shader.prepare(sampleCount());
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({{0.f, 0.f, 1.f, 1.f}});
        pass.draw(shader);
    }

    SampleShader shader;
};

Texture makeBlackTexture()
{
    static std::uint32_t pixels[textureSize * textureSize];

    for (auto& pixel: pixels)
        pixel = black;

    auto descriptor = TextureDescriptor {};
    descriptor.width = textureSize;
    descriptor.height = textureSize;
    descriptor.format = TextureFormat::RGBA8Unorm;

    return Device::shared().makeTexture(descriptor, pixels);
}

bool isRed(const Graphics::Color& c)
{
    return c.r > 0.5f && c.g < 0.5f && c.b < 0.5f;
}
bool isGreen(const Graphics::Color& c)
{
    return c.g > 0.5f && c.r < 0.5f;
}
bool isBlack(const Graphics::Color& c)
{
    return c.r < 0.5f && c.g < 0.5f && c.b < 0.5f;
}

int count(const Graphics::Image& image, bool (*predicate)(const Graphics::Color&))
{
    auto total = 0;

    for (auto y = 0; y < image.height(); ++y)
        for (auto x = 0; x < image.width(); ++x)
            if (predicate(image.at(x, y)))
                ++total;

    return total;
}

// Renders the texture 1:1 so one image pixel is one texel.
Graphics::Image readBack(Texture& texture)
{
    auto view = TextureView {texture};
    view.setBounds({0.f, 0.f, (float) textureSize, (float) textureSize});

    return view.renderToImage(1.f);
}
} // namespace

auto tReadBackBaseline = test("TextureRegion/readBackShowsInitialContents") = []
{
    if (!Device::shared().isValid())
        return;

    auto texture = makeBlackTexture();

    if (!texture.isValid())
        return;

    auto image = readBack(texture);

    check(image.isValid());
    check(image.width() == textureSize);
    check(count(image, isBlack) == textureSize * textureSize);
};

auto tRegionLandsAtOrigin = test("TextureRegion/uploadsAtTheGivenOrigin") = []
{
    if (!Device::shared().isValid())
        return;

    auto texture = makeBlackTexture();

    if (!texture.isValid())
        return;

    std::uint32_t column[2 * textureSize];

    for (auto& pixel: column)
        pixel = red;

    texture.update({0.f, 0.f, 2.f, (float) textureSize}, column);

    auto image = readBack(texture);
    check(image.isValid());

    for (auto y = 0; y < textureSize; ++y)
    {
        check(isRed(image.at(0, y)));
        check(isRed(image.at(1, y)));
        check(isBlack(image.at(2, y)));
        check(isBlack(image.at(textureSize - 1, y)));
    }

    check(count(image, isRed) == 2 * textureSize);
};

auto tRegionLeavesRestUntouched =
    test("TextureRegion/leavesTheRestOfTheTextureAlone") = []
{
    if (!Device::shared().isValid())
        return;

    auto texture = makeBlackTexture();

    if (!texture.isValid())
        return;

    std::uint32_t reds[2 * textureSize];
    std::uint32_t greens[2 * textureSize];

    for (auto i = 0; i < 2 * textureSize; ++i)
    {
        reds[i] = red;
        greens[i] = green;
    }

    texture.update({0.f, 0.f, 2.f, (float) textureSize}, reds);
    texture.update({6.f, 0.f, 2.f, (float) textureSize}, greens);

    auto image = readBack(texture);
    check(image.isValid());

    check(count(image, isRed) == 2 * textureSize);
    check(count(image, isGreen) == 2 * textureSize);
    check(count(image, isBlack) == 4 * textureSize);

    for (auto y = 0; y < textureSize; ++y)
    {
        check(isRed(image.at(0, y)));
        check(isGreen(image.at(7, y)));
        check(isBlack(image.at(4, y)));
    }
};

// Asserted by area, so this does not depend on which way up the read-back is.
auto tRegionHeightIsHonoured = test("TextureRegion/uploadsOnlyTheRegionHeight") = []
{
    if (!Device::shared().isValid())
        return;

    auto texture = makeBlackTexture();

    if (!texture.isValid())
        return;

    std::uint32_t band[textureSize * 2];

    for (auto& pixel: band)
        pixel = red;

    texture.update({0.f, 0.f, (float) textureSize, 2.f}, band);

    auto image = readBack(texture);
    check(image.isValid());
    check(count(image, isRed) == textureSize * 2);
};

// A 2-wide region read from a 4-wide source, the shape a glyph arrives in.
auto tRegionRespectsSourceStride = test("TextureRegion/respectsSourceStride") = []
{
    if (!Device::shared().isValid())
        return;

    auto texture = makeBlackTexture();

    if (!texture.isValid())
        return;

    const std::uint32_t wide[] = {
        red,
        red,
        green,
        green, // row 0
        red,
        red,
        green,
        green, // row 1
    };

    texture.update({0.f, 0.f, 2.f, 2.f}, wide, 4 * sizeof(std::uint32_t));

    auto image = readBack(texture);
    check(image.isValid());

    check(count(image, isRed) == 4);
    check(count(image, isGreen) == 0);
};

// Dropped whole rather than clamped: a clamped region would go on consuming
// source rows at the original width and write skewed pixels.
auto tRegionRejectsOutOfBounds = test("TextureRegion/rejectsOutOfBoundsRegions") = []
{
    if (!Device::shared().isValid())
        return;

    auto texture = makeBlackTexture();

    if (!texture.isValid())
        return;

    std::uint32_t pixels[textureSize * textureSize];

    for (auto& pixel: pixels)
        pixel = red;

    texture.update({(float) textureSize - 1.f, 0.f, 4.f, 4.f},
                   pixels); // past the right edge
    texture.update({0.f, 0.f, (float) textureSize + 1.f, 1.f}, pixels); // too wide
    texture.update({-2.f, 0.f, 4.f, 4.f}, pixels); // negative origin
    texture.update({100.f, 100.f, 2.f, 2.f}, pixels); // entirely outside

    auto image = readBack(texture);
    check(image.isValid());
    check(count(image, isRed) == 0);
    check(count(image, isBlack) == textureSize * textureSize);
};

auto tRegionIgnoresEmptyAndNull =
    test("TextureRegion/emptyRegionAndNullPixelsAreNoOps") = []
{
    if (!Device::shared().isValid())
        return;

    auto texture = makeBlackTexture();

    if (!texture.isValid())
        return;

    std::uint32_t pixels[textureSize * textureSize];

    for (auto& pixel: pixels)
        pixel = red;

    texture.update({2.f, 2.f, 0.f, 0.f}, pixels);
    texture.update({2.f, 2.f, 2.f, 0.f}, pixels);
    texture.update({2.f, 2.f, -4.f, 4.f}, pixels);
    texture.update({0.f, 0.f, 4.f, 4.f}, nullptr);

    auto image = readBack(texture);
    check(image.isValid());
    check(count(image, isRed) == 0);
    check(texture.isValid());
};

auto tFullRegionMatchesWholeUpdate =
    test("TextureRegion/fullRegionMatchesWholeUpdate") = []
{
    if (!Device::shared().isValid())
        return;

    auto texture = makeBlackTexture();

    if (!texture.isValid())
        return;

    std::uint32_t pixels[textureSize * textureSize];

    for (auto& pixel: pixels)
        pixel = green;

    texture.update({0.f, 0.f, (float) textureSize, (float) textureSize}, pixels);

    auto image = readBack(texture);
    check(image.isValid());
    check(count(image, isGreen) == textureSize * textureSize);
};
