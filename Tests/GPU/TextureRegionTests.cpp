#include "Common.h"

// Texture::update(region, ...) — the sub-rectangle upload path a glyph atlas
// depends on, where each new glyph must cost a transfer its own size rather
// than a re-upload of the whole atlas.
//
// There is no texture read-back API, so these tests get one the only way
// available: draw the texture 1:1 into an off-screen GPUView with Nearest
// sampling and read *that* back. One texel lands on one pixel, so the image is
// the texture's contents.
//
// Positional assertions are made on the x axis only, where neither backend
// mirrors. Everything the y axis would be needed for is asserted by counting
// texels instead, which is orientation-independent.
//
// Runs on both backends, and self-skips without a GPU device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto textureSize = 8;

constexpr std::uint32_t black = 0xff000000;
constexpr std::uint32_t red = 0xff0000ff;   // RGBA8, little-endian: A B G R
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
// Two triangles covering clip space, with uv running 0..1 across them.
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

// Draws the texture across the whole view, unfiltered, so the read-back is the
// texture's own texels.
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

bool isRed(const Graphics::Color& c) { return c.r > 0.5f && c.g < 0.5f && c.b < 0.5f; }
bool isGreen(const Graphics::Color& c) { return c.g > 0.5f && c.r < 0.5f; }
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

// The read-back path itself, before anything is written through it: a texture
// filled with black must come back black everywhere. Without this the other
// cases could pass on a broken sampler.
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

// A region covering the two left columns lands on exactly those columns.
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

// The whole point of the API: a second upload elsewhere must leave the first
// alone. A whole-texture update() would wipe the red.
auto tRegionLeavesRestUntouched = test("TextureRegion/leavesTheRestOfTheTextureAlone") = []
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

// Height is honoured too. Asserted by area rather than position, so the test
// does not depend on which way up the read-back arrives.
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

// Source rows may be a slice of a wider buffer, which is exactly how a glyph
// arrives out of a rasterization bitmap. Here a 2-wide region is read from a
// 4-wide source, so only the first two pixels of each row are taken.
auto tRegionRespectsSourceStride = test("TextureRegion/respectsSourceStride") = []
{
    if (!Device::shared().isValid())
        return;

    auto texture = makeBlackTexture();

    if (!texture.isValid())
        return;

    // 4 px per row, of which the first 2 are uploaded and the last 2 skipped.
    const std::uint32_t wide[] = {
        red, red, green, green, // row 0
        red, red, green, green, // row 1
    };

    texture.update({0.f, 0.f, 2.f, 2.f}, wide, 4 * sizeof(std::uint32_t));

    auto image = readBack(texture);
    check(image.isValid());

    // The greens sat outside the region's width and must not have been taken.
    check(count(image, isRed) == 4);
    check(count(image, isGreen) == 0);
};

// Out-of-bounds regions are dropped whole rather than clamped: a clamped region
// would keep consuming source rows at the original width and write skewed
// pixels, which is much harder to notice than nothing happening.
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

    texture.update({(float) textureSize - 1.f, 0.f, 4.f, 4.f}, pixels); // past the right edge
    texture.update({0.f, 0.f, (float) textureSize + 1.f, 1.f}, pixels); // too wide
    texture.update({-2.f, 0.f, 4.f, 4.f}, pixels);                      // negative origin
    texture.update({100.f, 100.f, 2.f, 2.f}, pixels);                   // entirely outside

    auto image = readBack(texture);
    check(image.isValid());
    check(count(image, isRed) == 0);
    check(count(image, isBlack) == textureSize * textureSize);
};

// Degenerate regions and a null source are safe no-ops, so a caller need not
// special-case an empty glyph.
auto tRegionIgnoresEmptyAndNull = test("TextureRegion/emptyRegionAndNullPixelsAreNoOps") = []
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

// A full-size region is equivalent to the whole-texture overload, which is how
// the two share one code path underneath.
auto tFullRegionMatchesWholeUpdate = test("TextureRegion/fullRegionMatchesWholeUpdate") = []
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
