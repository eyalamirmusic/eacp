#include "Common.h"

// The first pass writes (4, 2, 0), which only a float target can hold, and the
// second scales it down: kept-vs-clamped shows up as red and green differing.
// The reader writes opaque alpha, or the premultiplied read-back equalises them.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto viewWidth = 8;
constexpr auto viewHeight = 4;
constexpr auto targetSize = 4;

// Small enough that neither outcome reaches the drawable's own ceiling.
constexpr auto readerScale = 0.2f;

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

struct WriterShader final : ShaderProgram
{
    WriterShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);

        setPosition(float4(position, 0.f, 1.f));
        setFragment(float4(constant(4.f), 2.f, 0.f, 1.f));
    }

    EACP_SHADER()
};

struct ReaderShader final : ShaderProgram
{
    ReaderShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        auto uv = vertexInput(&QuadVertex::uv);

        setPosition(float4(position, 0.f, 1.f));
        setFragment(float4(sample(image, varying(uv)).xyz() * readerScale, 1.f));
    }

    Uniform<Texture2D> image;

    EACP_SHADER(image)
};

// Two passes on one frame: an earlier pass's target sampled in a later one,
// with no fence between them.
struct TwoPassView final : GPUView
{
    explicit TwoPassView(TextureFormat format)
        : target(Device::shared().makeTexture(describe(format)))
    {
        setSampleCount(1);

        writer.setVertices(fullQuad, 6);
        writer.prepare(1,
                       false,
                       PrimitiveTopology::Triangles,
                       BlendMode::None,
                       pixelFormatFor(format));

        reader.setVertices(fullQuad, 6);
        reader.image = target;
        reader.prepare(sampleCount());
    }

    static TextureDescriptor describe(TextureFormat format)
    {
        auto descriptor = TextureDescriptor {};
        descriptor.width = targetSize;
        descriptor.height = targetSize;
        descriptor.format = format;
        descriptor.renderTarget = true;
        return descriptor;
    }

    void render(Frame& frame) override
    {
        {
            auto into = frame.beginPass(target, {{0.f, 0.f, 0.f, 1.f}});

            if (drawIntoTarget)
                into.draw(writer);
        }

        auto pass = frame.beginPass({{0.f, 0.f, 1.f, 1.f}});
        pass.draw(reader);
    }

    Texture target;
    WriterShader writer;
    ReaderShader reader;

    // Off for the case that has to come back empty.
    bool drawIntoTarget = true;
};

Graphics::Image readBack(TwoPassView& view)
{
    view.setBounds({0.f, 0.f, (float) viewWidth, (float) viewHeight});
    return view.renderToImage(1.f);
}
} // namespace

auto tPlainTextureIsNotATarget = test("RenderTarget/aPlainTextureIsNotOne") = []
{
    if (!Device::shared().isValid())
        return;

    auto descriptor = TextureDescriptor {};
    descriptor.width = targetSize;
    descriptor.height = targetSize;

    auto plain = Device::shared().makeTexture(descriptor);

    check(plain.isValid());
    check(!plain.isRenderTarget());

    descriptor.renderTarget = true;
    auto target = Device::shared().makeTexture(descriptor);

    check(target.isValid());
    check(target.isRenderTarget());
};

auto tFloatTargetKeepsRange =
    test("RenderTarget/floatTargetKeepsWhatItWasGiven") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = TwoPassView {TextureFormat::RGBA16Float};

    if (!view.target.isRenderTarget())
        return;

    auto image = readBack(view);

    check(image.isValid());

    auto pixel = image.at(viewWidth / 2, viewHeight / 2);

    // Not the blue the drawable was cleared to, so the target was sampled.
    check(pixel.b < 0.25f);

    // Red above green only happens if 4 and 2 were both still there to scale.
    check(pixel.r > pixel.g + 0.2f);
};

// The control: (4, 2, 0) clamps to (1, 1, 0) going in, so red and green come
// back equal.
auto tUnormTargetClamps = test("RenderTarget/eightBitTargetLosesTheRange") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = TwoPassView {TextureFormat::RGBA8Unorm};

    if (!view.target.isRenderTarget())
        return;

    auto image = readBack(view);

    check(image.isValid());

    auto pixel = image.at(viewWidth / 2, viewHeight / 2);

    check(pixel.b < 0.25f);
    check(pixel.r > 0.1f);
    check(std::abs(pixel.r - pixel.g) < 0.1f);
};

auto tEmptyTargetSamplesItsClear = test("RenderTarget/anEmptyTargetIsItsClear") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = TwoPassView {TextureFormat::RGBA16Float};

    if (!view.target.isRenderTarget())
        return;

    view.drawIntoTarget = false;

    auto image = readBack(view);

    check(image.isValid());

    auto pixel = image.at(viewWidth / 2, viewHeight / 2);

    check(pixel.r < 0.1f);
    check(pixel.g < 0.1f);
    check(pixel.b < 0.25f);
};
