#include "Common.h"

// Depth testing inside a pass that renders into a texture
// (TextureDescriptor::depth). Without it a 3D scene cannot go into a texture at
// all - the depth test has nothing to test against, so what survives is
// painter's order.
//
// The scene is two full-screen quads at different depths, drawn in the order
// only a depth test gets right: the near one first, then the far one over it.
// With depth the near quad's colour remains; without it the far quad painted
// over it and its colour does. The two cases differ in one flag and come back
// different colours, which is what makes either of them evidence - a target
// with no depth buffer is not a broken target, it is the same pass with no test
// in it, and the control says so rather than assuming it.
//
// The target is sampled by a second pass on the same frame and read back off the
// drawable, because a render target has no CPU-side observable of its own.
//
// What these do NOT distinguish, measured rather than assumed: whether
// Frame::beginPass attached the buffer. Nulling the depth handle so the pass
// gets no attachment at all leaves this file green on Apple silicon - the tile
// memory is there either way and the hardware goes on depth-testing - while
// Metal's validation layer reports "MTLDepthStencilDescriptor sets depth test
// but MTLRenderPassDescriptor has a nil depthAttachment texture" for every draw.
// That is undefined behaviour that happens to work on one architecture. The
// D3D12 path, where OMSetRenderTargets with a null DSV genuinely disables the
// test, fails it properly. So when changing the attachment, run these under
// MTL_DEBUG_LAYER=1: a silent validation layer is the other half of the
// evidence.
//
// Runs on both backends, and self-skips without a GPU device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto viewWidth = 8;
constexpr auto viewHeight = 4;
constexpr auto targetSize = 4;

// Clip-space z for the two quads. Both inside [0, 1] with w = 1, so they reach
// the depth buffer as they stand and near really is nearer.
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

// One quad, at the depth and in the colour its uniforms give it. The same
// program draws both, rebound between them, so nothing but z and colour
// separates the two draws.
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

// Draws the two quads into the target, then the target onto the drawable.
struct DepthTargetView final : GPUView
{
    explicit DepthTargetView(bool wantDepth)
        : target(Device::shared().makeTexture(describe(wantDepth)))
    {
        setSampleCount(1);

        quads.setVertices(fullQuad, 6);

        // A texture pass never multisamples, so a pipeline drawing into one is
        // single-sampled whatever the view is. The depth flag has to match the
        // target: a pipeline declaring a depth attachment the pass does not have
        // is a validation error rather than an untested draw.
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

// A target says whether it carries one, so a pipeline can be built to match
// rather than finding out at the draw.
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

// Depth is only meaningful for something that renders, so asking for it on a
// plain texture is ignored rather than allocating a buffer nothing can attach.
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

// The near quad was drawn first and survives the far one drawn over it, which
// only a depth test does.
auto tDepthKeepsTheNearerQuad =
    test("RenderTargetDepth/theNearerSurfaceSurvivesTheFartherOne") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = DepthTargetView {true};

    // Asserted rather than skipped past: a target that quietly came back with no
    // depth buffer is a failure this test is here to catch, and skipping on it
    // would report success for it.
    check(view.target.hasDepth());

    auto pixel = centrePixel(view);

    check(pixel.r > 0.5f);
    check(pixel.g < 0.5f);
};

// The control, and the reason the case above is evidence: the same two draws
// with no depth buffer under them leave the last one drawn, so the colours come
// back the other way round.
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
