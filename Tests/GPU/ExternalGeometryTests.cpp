#include "Common.h"

// RenderPass::bind - a program drawn over geometry the app owns rather than
// geometry the program owns.
//
// draw(program) assumes both halves of a draw belong to the program: its vertex
// buffer and its whole vertex count. A renderer whose geometry is one buffer it
// updates in place and issues as several sub-ranges - one run per texture, one
// batch per flush - can supply neither, and before bind() existed its only route
// was to inline draw(program)'s body and keep the copy in step by hand.
//
// So what these check is not that a triangle appeared. It is that binding a
// program by hand still binds *everything* draw(program) binds, that the
// geometry comes from the caller's buffer and not the program's, and that a
// sub-range is the sub-range asked for. The last is the one an eyeball misses:
// a firstVertex quietly ignored draws the whole buffer, which looks like a
// picture rather than like a bug.
//
// Everything renders off-screen through View::renderToImage, so it runs in CI on
// both backends, and self-skips without a GPU device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
struct Vertex
{
    float position[2];
};
} // namespace

EACP_SHADER_VALUE(Vertex, Float2)

namespace
{
// Two triangles in one buffer, each covering one half of clip space, so a draw
// that took the wrong range or the wrong buffer paints the wrong half and the
// right half keeps the clear colour.
constexpr Vertex halves[] = {
    {{-1.f, -1.f}},
    {{0.f, -1.f}},
    {{-1.f, 3.f}},

    {{0.f, -1.f}},
    {{1.f, -1.f}},
    {{1.f, 3.f}},
};

constexpr auto leftRange = 0;
constexpr auto rightRange = 3;
constexpr auto rangeVertices = 3;

// Flat colour from a uniform, so what reaches the pixel says whether the uniform
// block was bound - and, drawn twice with the uniform changed in between,
// whether a caller can restate per-draw state after one bind().
struct FlatShader final : ShaderProgram
{
    FlatShader() { compile(); }

    void define() override
    {
        setPosition(float4(vertexInput(&Vertex::position), 0.f, 1.f));
        setFragment(color);
    }

    Uniform<Float4> color;

    EACP_SHADER(color)
};

// The same, sampling a texture instead: bindTextures is the one thing a run loop
// restates per draw, so it needs a case where the two draws disagree about it.
struct TexturedShader final : ShaderProgram
{
    TexturedShader() { compile(); }

    void define() override
    {
        setPosition(float4(vertexInput(&Vertex::position), 0.f, 1.f));

        // The texture is one texel, so where it is sampled does not matter -
        // but a float2 wants at least one shader value to hang off, the EDSL
        // building expressions rather than constant-folding literals.
        setFragment(sample(image, float2(constant(0.5f), 0.5f)));
    }

    Uniform<Texture2D> image;

    EACP_SHADER(image)
};

Texture solidTexture(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = 1;
    descriptor.height = 1;

    const std::uint8_t pixel[] = {r, g, b, 255};

    return Device::shared().makeTexture(descriptor, pixel);
}

constexpr auto clearBlue = Graphics::Color {0.f, 0.f, 1.f, 1.f};

bool isRed(const Graphics::Color& c)
{
    return c.r > 0.5f && c.g < 0.5f && c.b < 0.5f;
}

bool isGreen(const Graphics::Color& c)
{
    return c.g > 0.5f && c.r < 0.5f && c.b < 0.5f;
}

bool isBlue(const Graphics::Color& c)
{
    return c.b > 0.5f && c.r < 0.5f && c.g < 0.5f;
}

// The app's geometry: a Buffer this view owns, which no program is ever told
// about.
struct ExternalGeometryView : GPUView
{
    ExternalGeometryView()
        : geometry(Device::shared(), halves, sizeof(halves), BufferUsage::Vertex)
    {
        setSampleCount(1);
    }

    Buffer geometry;
};

// bind() once, then two draws that differ only in a uniform - the shape a run
// loop over one buffer takes.
struct TwoRangeView final : ExternalGeometryView
{
    TwoRangeView() { shader.prepare(sampleCount(), false); }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({clearBlue});

        pass.bind(shader, geometry);

        shader.color = Array {1.f, 0.f, 0.f, 1.f};
        pass.setUniforms(shader);
        pass.draw(rangeVertices, leftRange);

        shader.color = Array {0.f, 1.f, 0.f, 1.f};
        pass.setUniforms(shader);
        pass.draw(rangeVertices, rightRange);
    }

    FlatShader shader;
};

// The one-shot overload, drawing a single range and nothing else.
struct OneRangeView final : ExternalGeometryView
{
    OneRangeView() { shader.prepare(sampleCount(), false); }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({clearBlue});

        // The *second* range deliberately: drawn from the first, a firstVertex
        // that never reached the draw call would paint exactly the same picture
        // and this case would pass through the bug it exists for.
        shader.color = Array {1.f, 0.f, 0.f, 1.f};
        pass.draw(shader, geometry, rangeVertices, rightRange);
    }

    FlatShader shader;
};

// bind() once, then two draws that differ in their texture.
struct TwoTextureView final : ExternalGeometryView
{
    TwoTextureView()
        : red(solidTexture(255, 0, 0))
        , green(solidTexture(0, 255, 0))
    {
        shader.prepare(sampleCount(), false);
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({clearBlue});

        shader.image = red;
        pass.bind(shader, geometry);
        pass.draw(rangeVertices, leftRange);

        shader.image = green;
        shader.bindTextures(pass);
        pass.draw(rangeVertices, rightRange);
    }

    Texture red;
    Texture green;
    TexturedShader shader;
};

template <typename View>
bool ready(View& view)
{
    if (!Device::shared().isValid() || !view.shader.pipeline().isValid())
        return false;

    view.setBounds({0.f, 0.f, 32.f, 32.f});
    return true;
}
} // namespace

// The whole of it in one picture: the caller's buffer supplied the geometry, the
// uniform block was bound by bind() and rebindable after it, and each draw took
// the range it named rather than the buffer.
auto tTwoRanges = test("ExternalGeometry/bindDrawsTheRangesAsked") = []
{
    auto view = TwoRangeView {};

    if (!ready(view))
        return;

    auto image = view.renderToImage(1.f);
    check(image.isValid());

    check(isRed(image.at(8, 16)), "the first range painted the left half red");
    check(isGreen(image.at(24, 16)), "the second range painted the right half green");
};

// The property a program that owns no geometry rests on, stated where it can
// fail: bind(program, vertices) must not ask the program for a buffer, because
// a program only ever drawn this way has never been given one.
auto tOwnsNoVertices = test("ExternalGeometry/programNeedsNoBufferOfItsOwn") = []
{
    auto view = TwoRangeView {};

    if (!ready(view))
        return;

    check(!view.shader.hasVertices(),
          "the program was never handed geometry of its own");

    auto image = view.renderToImage(1.f);
    check(image.isValid());
    check(isRed(image.at(8, 16)), "and it drew anyway");
};

// firstVertex, on its own. Drawing one range must leave the other half of the
// frame at the clear colour - the check a "did something appear" test cannot
// make, and the one a silently ignored firstVertex fails.
auto tOneRange = test("ExternalGeometry/drawOverAppBufferTakesOneRange") = []
{
    auto view = OneRangeView {};

    if (!ready(view))
        return;

    auto image = view.renderToImage(1.f);
    check(image.isValid());

    check(isRed(image.at(24, 16)), "the range asked for was drawn");
    check(isBlue(image.at(8, 16)), "and nothing else was");
};

// The run loop's real per-draw state. bind() binds the textures once; a caller
// changing one between draws restates it itself, which is what bindTextures is
// public for.
auto tTwoTextures = test("ExternalGeometry/texturesRebindBetweenDraws") = []
{
    auto view = TwoTextureView {};

    if (!ready(view))
        return;

    auto image = view.renderToImage(1.f);
    check(image.isValid());

    check(isRed(image.at(8, 16)), "the first range sampled the texture bind() bound");
    check(isGreen(image.at(24, 16)), "the second sampled the one rebound after it");
};
