#include "Common.h"

// Rendering into a texture and then sampling it, on one frame. Two things are
// being pinned and neither has a CPU-side observable:
//
// That the pass reached the texture at all. A target that never got drawn into
// samples as whatever it was cleared to, which is a picture like any other -
// only the frame says which of the two happened.
//
// That a float format keeps what an 8-bit one throws away. This is the reason a
// feedback buffer needs one: eight bits per channel cannot hold a value above 1,
// so a pass that accumulates anything - a trail, a simulation state - loses the
// range every frame and settles into a flat colour. The first pass here writes
// (4, 2, 0), which only a float target can hold; the second scales it down and
// shows (0.8, 0.4, 0) if the range survived and (0.2, 0.2, 0) if it clamped.
//
// The check is that red and green *differ*, not that they hit particular
// values, so it says the same thing whatever transfer function the frame comes
// back through. Clamped, the two channels are equal.
//
// The second pass writes an opaque alpha deliberately. The snapshot read-back
// hands back what Core Animation composites, which is premultiplied, so a
// fragment left at alpha 0.25 comes back with its colour divided by four - and
// two colours that differed before that division can arrive equal after it.
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

// Small enough that neither outcome reaches the drawable's own ceiling, so what
// the frame shows is the target's range rather than a second clamp.
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

// What goes into the target: a colour no 8-bit format can hold.
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

// What comes out of it, scaled so the difference between kept and clamped is
// the difference between two channels of one pixel.
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

// Draws the writer into the target, then the reader into the drawable - two
// passes on one frame, which is what says a texture written by an earlier pass
// is legal to sample in a later one without a fence between them.
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

    // Off for the case that has to come back empty: the target is bound and
    // sampled with nothing ever drawn into it.
    bool drawIntoTarget = true;
};

Graphics::Image readBack(TwoPassView& view)
{
    view.setBounds({0.f, 0.f, (float) viewWidth, (float) viewHeight});
    return view.renderToImage(1.f);
}
} // namespace

// A texture created without the flag cannot be rendered into, and says so rather
// than leaving the pass to find out.
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

// The float target keeps (4, 2, 0), so a fifth of it is (0.8, 0.4, 0) and the
// two channels differ.
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

    // Not the blue the drawable was cleared to, which is what says the second
    // pass sampled the target rather than nothing reaching it.
    check(pixel.b < 0.25f);

    // And the range survived: red came back above green rather than level with
    // it, which only happens if 4 and 2 were both still there to be scaled.
    check(pixel.r > pixel.g + 0.2f);
};

// The same passes over an 8-bit target, which is the control: (4, 2, 0) clamps
// to (1, 1, 0) on the way in, so a fifth of it has red and green equal. If this
// case also came back with them apart, the one above would be measuring nothing.
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

// Nothing drawn into the target is the target's clear colour, not the last
// frame's contents and not whatever the drawable happened to hold.
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
