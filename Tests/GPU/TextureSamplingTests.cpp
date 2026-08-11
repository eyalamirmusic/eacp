#include "Common.h"

// Regression: Metal ignored the shader's sampler declaration and read the
// Texture's own, so a shader declaring Repeat tiled on Windows and clamped on
// macOS. See Lib/eacp/GPU/SAMPLERS.md.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto viewWidth = 16;
constexpr auto viewHeight = 4;

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
// u runs 0..1, so each texel covers half the width.
constexpr QuadVertex unitQuad[] = {
    {{-1.f, -1.f}, {0.f, 1.f}},
    {{1.f, -1.f}, {1.f, 1.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
    {{1.f, -1.f}, {1.f, 1.f}},
    {{1.f, 1.f}, {1.f, 0.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
};

// u runs 1..2, entirely outside the texture: Clamp holds the last texel across
// the width, Repeat draws the texture again. Nothing else separates the two.
constexpr QuadVertex wrappedQuad[] = {
    {{-1.f, -1.f}, {1.f, 1.f}},
    {{1.f, -1.f}, {2.f, 1.f}},
    {{-1.f, 1.f}, {1.f, 0.f}},
    {{1.f, -1.f}, {2.f, 1.f}},
    {{1.f, 1.f}, {2.f, 0.f}},
    {{-1.f, 1.f}, {1.f, 0.f}},
};

struct SamplingShader final : ShaderProgram
{
    explicit SamplingShader(TextureSampling sampling)
    {
        image.sampling = sampling;
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

struct SamplingView final : GPUView
{
    SamplingView(Texture& textureToShow,
                 TextureSampling sampling,
                 const QuadVertex* quad)
        : shader(sampling)
    {
        setSampleCount(1);

        shader.setVertices(quad, 6);
        shader.image = textureToShow;
        shader.prepare(sampleCount());
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({{0.f, 0.f, 0.f, 1.f}});
        pass.draw(shader);
    }

    SamplingShader shader;
};

// Red on the left, green on the right.
Texture makeTwoTexelTexture()
{
    static std::uint32_t pixels[] = {red, green};

    auto descriptor = TextureDescriptor {};
    descriptor.width = 2;
    descriptor.height = 1;
    descriptor.format = TextureFormat::RGBA8Unorm;

    return Device::shared().makeTexture(descriptor, pixels);
}

Graphics::Image
    readBack(Texture& texture, TextureSampling sampling, const QuadVertex* quad)
{
    auto view = SamplingView {texture, sampling, quad};
    view.setBounds({0.f, 0.f, (float) viewWidth, (float) viewHeight});

    return view.renderToImage(1.f);
}

bool isRed(const Graphics::Color& c)
{
    return c.r > 0.5f && c.g < 0.5f;
}
bool isGreen(const Graphics::Color& c)
{
    return c.g > 0.5f && c.r < 0.5f;
}

// Neither source colour, so only interpolation produces one.
bool isBlended(const Graphics::Color& c)
{
    return c.r > 0.2f && c.r < 0.8f && c.g > 0.2f && c.g < 0.8f;
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
} // namespace

auto tBothTexelsDrawn = test("TextureSampling/bothTexelsAreDrawn") = []
{
    if (!Device::shared().isValid())
        return;

    auto texture = makeTwoTexelTexture();

    if (!texture.isValid())
        return;

    auto image = readBack(
        texture, {TextureFilter::Nearest, TextureAddressMode::Clamp}, unitQuad);

    check(image.isValid());
    check(count(image, isRed) > 0);
    check(count(image, isGreen) > 0);
};

// A backend that ignores the declaration and clamps anyway also passes this,
// which is why the Repeat case carries the weight.
auto tClampHoldsTheEdge = test("TextureSampling/clampHoldsTheLastTexel") = []
{
    if (!Device::shared().isValid())
        return;

    auto texture = makeTwoTexelTexture();

    if (!texture.isValid())
        return;

    auto image = readBack(
        texture, {TextureFilter::Nearest, TextureAddressMode::Clamp}, wrappedQuad);

    check(image.isValid());
    check(count(image, isGreen) == viewWidth * viewHeight);
    check(count(image, isRed) == 0);
};

// The case that proves the declaration reached the sampler: a backend reading
// it from anywhere else gets the default Clamp and comes back with no red.
auto tRepeatWrapsAround = test("TextureSampling/repeatWrapsBackToTheFirstTexel") = []
{
    if (!Device::shared().isValid())
        return;

    auto texture = makeTwoTexelTexture();

    if (!texture.isValid())
        return;

    auto image = readBack(
        texture, {TextureFilter::Nearest, TextureAddressMode::Repeat}, wrappedQuad);

    check(image.isValid());
    check(count(image, isRed) > 0);
    check(count(image, isGreen) > 0);
};

auto tNearestDoesNotBlend = test("TextureSampling/nearestKeepsTexelsDistinct") = []
{
    if (!Device::shared().isValid())
        return;

    auto texture = makeTwoTexelTexture();

    if (!texture.isValid())
        return;

    auto image = readBack(
        texture, {TextureFilter::Nearest, TextureAddressMode::Clamp}, unitQuad);

    check(image.isValid());
    check(count(image, isBlended) == 0);
};

auto tLinearBlends = test("TextureSampling/linearBlendsBetweenTexels") = []
{
    if (!Device::shared().isValid())
        return;

    auto texture = makeTwoTexelTexture();

    if (!texture.isValid())
        return;

    auto image = readBack(
        texture, {TextureFilter::Linear, TextureAddressMode::Clamp}, unitQuad);

    check(image.isValid());
    check(count(image, isBlended) > 0);
};
