#include "Common.h"

// The TextureSampling a shader declares must actually reach the GPU. The two
// backends get there by completely different routes — Metal binds the
// MTLSamplerState the declaration names, D3D12 selects a static sampler by the
// register the emitter puts the SamplerState on — so nothing but a rendered
// result checks both.
//
// This is worth pinning because the failure mode is silent. Metal ignored the
// declaration entirely for a while and read the Texture's own sampler instead:
// nothing errored, no test failed, and the only symptom was that a shader
// declaring Repeat tiled on Windows and clamped on macOS. See
// Lib/eacp/GPU/SAMPLERS.md.
//
// Each case draws a two-texel red|green texture through a shader declaring one
// configuration and reads the pixels back. The address mode is checked with UVs
// that leave [0, 1], which is the only place Clamp and Repeat differ; the
// filter is checked by looking for blended pixels, which only Linear produces.
//
// Runs on both backends, and self-skips without a GPU device.

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
// A full-viewport quad whose u runs 0..1: each texel covers half the width.
constexpr QuadVertex unitQuad[] = {
    {{-1.f, -1.f}, {0.f, 1.f}},
    {{1.f, -1.f}, {1.f, 1.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
    {{1.f, -1.f}, {1.f, 1.f}},
    {{1.f, 1.f}, {1.f, 0.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
};

// The same quad with u running 1..2 — entirely outside the texture. Clamp holds
// the last texel across the whole width; Repeat wraps and draws the texture
// again. Nothing else distinguishes the two.
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

// Two texels side by side: red on the left, green on the right.
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

// Neither source colour: only interpolation between the two texels makes one,
// so this is what separates Linear from Nearest.
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

// The baseline the other cases rest on: with u across 0..1 the texture is drawn
// once, so both texels are on screen. If this fails the read-back or the quad is
// wrong and nothing below means anything.
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

// Clamp, sampled entirely past the right edge: every pixel is the last texel.
// Under a backend that ignores the declaration and clamps anyway this passes,
// which is why the Repeat case below is the one that carries the weight.
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

// Repeat over the same out-of-range UVs draws the texture again, so the red
// texel reappears. This is the case that actually proves the shader's
// declaration reached the sampler: a backend reading sampling from anywhere
// else gets the default Clamp here and comes back with no red at all.
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

// Nearest snaps to one texel or the other, so the boundary is hard: no pixel
// holds a mixture of the two colours.
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

// Linear interpolates between the texel centres, so the pixels between them are
// a mixture. The counterpart to the Nearest case: together they pin the filter
// axis of the declaration the way Clamp/Repeat pins the address axis.
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
