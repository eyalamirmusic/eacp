#include "Common.h"

#include <optional>

// A texture destroyed after a draw has sampled it, while the frame that drew
// it is still being recorded, must still be what that draw shows. A glyph
// atlas does exactly this when it grows mid-frame: the earlier batches of
// text were drawn from the old texture, which goes, and the next batch makes
// a bigger one.
//
// The draw is recorded and the GPU reads what it names only when the frame
// executes, so everything the draw names has to outlive the recording, not
// just the texture's memory: on D3D12 that includes the slot its view
// occupies in the shader-visible heap, which a texture made straight after
// would otherwise be handed and write its own view into.
//
// Self-skips without a GPU device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace TextureLifetimeTests
{
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
} // namespace TextureLifetimeTests

EACP_SHADER_VALUE(TextureLifetimeTests::QuadVertex, Float2)

namespace TextureLifetimeTests
{
namespace
{
constexpr QuadVertex leftHalf[] = {
    {{-1.f, -1.f}, {0.f, 1.f}},
    {{0.f, -1.f}, {1.f, 1.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
    {{0.f, -1.f}, {1.f, 1.f}},
    {{0.f, 1.f}, {1.f, 0.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
};

constexpr QuadVertex rightHalf[] = {
    {{0.f, -1.f}, {0.f, 1.f}},
    {{1.f, -1.f}, {1.f, 1.f}},
    {{0.f, 1.f}, {0.f, 0.f}},
    {{1.f, -1.f}, {1.f, 1.f}},
    {{1.f, 1.f}, {1.f, 0.f}},
    {{0.f, 1.f}, {0.f, 0.f}},
};

struct TexturedQuad final : ShaderProgram
{
    explicit TexturedQuad(const QuadVertex* quad)
    {
        compile();
        setVertices(quad, 6);
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

Texture solidTexture(std::uint32_t colour)
{
    const std::uint32_t pixel[] = {colour};

    auto descriptor = TextureDescriptor {};
    descriptor.width = 1;
    descriptor.height = 1;
    descriptor.format = TextureFormat::RGBA8Unorm;

    return Device::shared().makeTexture(descriptor, pixel);
}

// The left half drawn from a red texture, which is then destroyed and a green
// one made in its place for the right half, all in one frame.
struct ReplacingView final : GPUView
{
    ReplacingView()
    {
        setSampleCount(1);
        left.prepare(sampleCount());
        right.prepare(sampleCount());
        setBounds({0.f, 0.f, (float) viewWidth, (float) viewHeight});
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({{0.f, 0.f, 0.f, 1.f}});

        auto first = std::optional<Texture> {solidTexture(red)};
        left.image = *first;
        pass.draw(left);
        first.reset();

        replacement.emplace(solidTexture(green));
        right.image = *replacement;
        pass.draw(right);
    }

    TexturedQuad left {leftHalf};
    TexturedQuad right {rightHalf};
    std::optional<Texture> replacement;
};

bool isRed(const Graphics::Color& c)
{
    return c.r > 0.5f && c.g < 0.5f;
}

bool isGreen(const Graphics::Color& c)
{
    return c.g > 0.5f && c.r < 0.5f;
}
} // namespace

auto tDestroyedTextureKeepsItsDraw =
    test("TextureLifetime/aTextureDestroyedMidFrameStillShowsInItsDraw") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = ReplacingView {};
    auto image = view.renderToImage(1.f);

    check(image.isValid());

    if (!image.isValid())
        return;

    auto middle = image.height() / 2;

    check(isRed(image.at(image.width() / 4, middle)));
    check(isGreen(image.at(image.width() * 3 / 4, middle)));
};
} // namespace TextureLifetimeTests
