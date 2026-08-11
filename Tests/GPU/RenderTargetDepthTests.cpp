#include "Common.h"

// These cannot tell whether beginPass attached the buffer: Apple silicon
// depth-tests either way and only Metal's validation layer complains, so run
// them under MTL_DEBUG_LAYER=1 when changing the attachment.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto viewWidth = 8;
constexpr auto viewHeight = 4;
constexpr auto targetSize = 4;

// Clip-space z, both inside [0, 1] with w = 1.
constexpr auto nearDepth = 0.25f;
constexpr auto farDepth = 0.75f;

struct QuadVertex
{
    float position[2];
    float uv[2];
};
} // namespace

EACP_SHADER_VALUE(QuadVertex, Float2)

namespace
{
constexpr QuadVertex fullQuad[] = {
    {{-1.f, -1.f}, {0.f, 1.f}},
    {{1.f, -1.f}, {1.f, 1.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
    {{1.f, -1.f}, {1.f, 1.f}},
    {{1.f, 1.f}, {1.f, 0.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
};

struct DepthQuadShader final : ShaderProgram
{
    DepthQuadShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);

        setPosition(float4(position, depth, 1.f));
        setFragment(float4(color, 1.f));
    }

    Uniform<Float> depth;
    Uniform<Float3> color;

    EACP_SHADER(depth, color)
};

struct ReaderShader final : ShaderProgram
{
    ReaderShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        auto uv = vertexInput(&QuadVertex::uv);

        setPosition(float4(position, 0.f, 1.f));
        setFragment(float4(sample(image, varying(uv)).xyz(), 1.f));
    }

    Uniform<Texture2D> image;

    EACP_SHADER(image)
};

struct DepthTargetView final : GPUView
{
    explicit DepthTargetView(bool wantDepth)
        : target(Device::shared().makeTexture(describe(wantDepth)))
    {
        setSampleCount(1);

        quads.setVertices(fullQuad, 6);

        // A texture pass never multisamples, and the depth flag has to match the
        // target or the pipeline is a validation error rather than an untested
        // draw.
        quads.prepare(1,
                      wantDepth,
                      PrimitiveTopology::Triangles,
                      BlendMode::None,
                      pixelFormatFor(TextureFormat::RGBA8Unorm));

        reader.setVertices(fullQuad, 6);
        reader.image = target;
        reader.prepare(sampleCount());
    }

    static TextureDescriptor describe(bool wantDepth)
    {
        auto descriptor = TextureDescriptor {};
        descriptor.width = targetSize;
        descriptor.height = targetSize;
        descriptor.format = TextureFormat::RGBA8Unorm;
        descriptor.renderTarget = true;
        descriptor.depth = wantDepth;
        return descriptor;
    }

    void render(Frame& frame) override
    {
        {
            auto into = frame.beginPass(target, {{0.f, 0.f, 0.f, 1.f}});

            quads.depth = nearDepth;
            quads.color = std::array {1.f, 0.f, 0.f};
            into.draw(quads);

            quads.depth = farDepth;
            quads.color = std::array {0.f, 1.f, 0.f};
            into.draw(quads);
        }

        auto pass = frame.beginPass({{0.f, 0.f, 1.f, 1.f}});
        pass.draw(reader);
    }

    Texture target;
    DepthQuadShader quads;
    ReaderShader reader;
};

Graphics::Color centrePixel(DepthTargetView& view)
{
    view.setBounds({0.f, 0.f, (float) viewWidth, (float) viewHeight});
    auto image = view.renderToImage(1.f);

    check(image.isValid());

    return image.at(viewWidth / 2, viewHeight / 2);
}
} // namespace

auto tTargetReportsItsDepth =
    test("RenderTargetDepth/aTargetSaysWhetherItHasOne") = []
{
    if (!Device::shared().isValid())
        return;

    auto descriptor = TextureDescriptor {};
    descriptor.width = targetSize;
    descriptor.height = targetSize;
    descriptor.renderTarget = true;

    auto plain = Device::shared().makeTexture(descriptor);

    check(plain.isRenderTarget());
    check(!plain.hasDepth());

    descriptor.depth = true;
    auto withDepth = Device::shared().makeTexture(descriptor);

    check(withDepth.isRenderTarget());
    check(withDepth.hasDepth());
};

auto tDepthNeedsARenderTarget =
    test("RenderTargetDepth/aPlainTextureGetsNoDepthBuffer") = []
{
    if (!Device::shared().isValid())
        return;

    auto descriptor = TextureDescriptor {};
    descriptor.width = targetSize;
    descriptor.height = targetSize;
    descriptor.depth = true;

    auto plain = Device::shared().makeTexture(descriptor);

    check(plain.isValid());
    check(!plain.hasDepth());
};

auto tDepthKeepsTheNearerQuad =
    test("RenderTargetDepth/theNearerSurfaceSurvivesTheFartherOne") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = DepthTargetView {true};

    // Asserted rather than skipped: a target that came back without a depth
    // buffer is the failure this catches.
    check(view.target.hasDepth());

    auto pixel = centrePixel(view);

    check(pixel.r > 0.5f);
    check(pixel.g < 0.5f);
};

// The control that makes the case above evidence.
auto tWithoutDepthTheLastDrawWins =
    test("RenderTargetDepth/withoutADepthBufferPaintersOrderWins") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = DepthTargetView {false};

    check(!view.target.hasDepth());

    auto pixel = centrePixel(view);

    check(pixel.g > 0.5f);
    check(pixel.r < 0.5f);
};
