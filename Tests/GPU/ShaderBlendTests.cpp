#include "Common.h"

#include <cmath>

// ShaderProgram::prepare's blendMode.
//
// Checked by drawing, not by inspecting state: a blend mode has no CPU-side
// observable, and "the pipeline built" says nothing about whether blending
// happened. So each case renders a translucent quad over an opaque background
// off-screen and reads the result back.
//
// Runs on both backends; self-skips without a GPU device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
struct QuadVertex
{
    float position[2];
};
} // namespace

EACP_SHADER_VALUE(QuadVertex, Float2)

namespace
{
constexpr QuadVertex fullScreenTriangle[] = {{{-1.f, -1.f}},
                                             {{3.f, -1.f}},
                                             {{-1.f, 3.f}}};

// Emits a half-transparent red fragment, so the outcome depends entirely on
// whether the pipeline blends it with what is already there.
struct TranslucentShader final : ShaderProgram
{
    TranslucentShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);

        setPosition(float4(position, 0.f, 1.f));
        setFragment(color);
    }

    // Carried as a uniform rather than written as a literal: the EDSL builds
    // expressions from shader values, so a fully constant float4 has nothing to
    // hang off.
    Uniform<Float4> color;

    EACP_SHADER(color)
};

// Clears to opaque green, then draws the translucent red over it.
struct BlendView final : GPUView
{
    explicit BlendView(BlendMode modeToUse)
        : mode(modeToUse)
    {
        setSampleCount(1);
        shader.color = std::array {1.f, 0.f, 0.f, 0.5f};
        shader.setVertices(fullScreenTriangle);
        shader.prepare(sampleCount(), false, PrimitiveTopology::Triangles, mode);
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({{0.f, 1.f, 0.f, 1.f}});
        pass.draw(shader);
    }

    BlendMode mode;
    TranslucentShader shader;
};

bool near(float value, float target, float tolerance = 0.06f)
{
    return std::abs(value - target) <= tolerance;
}
} // namespace

// The default. A translucent fragment overwrites the background wholesale, so
// the green is gone and the red arrives at full strength despite its alpha.
auto tNoBlendOverwrites = test("ShaderBlend/defaultModeOverwritesTheBackground") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = BlendView {BlendMode::None};

    if (!view.shader.pipeline().isValid())
        return;

    view.setBounds({0.f, 0.f, 16.f, 16.f});

    auto image = view.renderToImage(1.f);
    check(image.isValid());

    const auto pixel = image.at(8, 8);

    check(near(pixel.r, 1.f));
    check(near(pixel.g, 0.f));
};

// With AlphaBlend the same fragment mixes: half red over green lands halfway
// between them. This is the case glyph coverage depends on.
auto tAlphaBlendMixes = test("ShaderBlend/alphaBlendMixesWithTheBackground") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = BlendView {BlendMode::AlphaBlend};

    if (!view.shader.pipeline().isValid())
        return;

    view.setBounds({0.f, 0.f, 16.f, 16.f});

    auto image = view.renderToImage(1.f);
    check(image.isValid());

    const auto pixel = image.at(8, 8);

    // Half of each, rather than all of one.
    check(pixel.r > 0.2f && pixel.r < 0.8f);
    check(pixel.g > 0.2f && pixel.g < 0.8f);
};

// Additive sums instead of mixing, so the green survives at full strength and
// the red is added on top.
auto tAdditiveAdds = test("ShaderBlend/additiveAddsToTheBackground") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = BlendView {BlendMode::Additive};

    if (!view.shader.pipeline().isValid())
        return;

    view.setBounds({0.f, 0.f, 16.f, 16.f});

    auto image = view.renderToImage(1.f);
    check(image.isValid());

    const auto pixel = image.at(8, 8);

    check(pixel.g > 0.9f);  // the background is untouched
    check(pixel.r > 0.2f);  // and the fragment was added to it
};

// The parameter is defaulted, so existing callers keep the unblended behaviour
// they were compiled against.
auto tDefaultsToNoBlend = test("ShaderBlend/prepareDefaultsToNoBlending") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = BlendView {BlendMode::None};

    if (!view.shader.pipeline().isValid())
        return;

    // Re-prepare without naming a blend mode at all.
    view.shader.prepare(view.sampleCount());
    view.setBounds({0.f, 0.f, 16.f, 16.f});

    auto image = view.renderToImage(1.f);
    check(image.isValid());
    check(near(image.at(8, 8).r, 1.f));
};
