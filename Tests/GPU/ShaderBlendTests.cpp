#include "Common.h"

#include <cmath>

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
constexpr QuadVertex fullScreenTriangle[] = {
    {{-1.f, -1.f}}, {{3.f, -1.f}}, {{-1.f, 3.f}}};

struct TranslucentShader final : ShaderProgram
{
    TranslucentShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);

        setPosition(float4(position, 0.f, 1.f));
        setFragment(color);
    }

    // A uniform rather than a literal: the EDSL builds expressions from shader
    // values, so a fully constant float4 has nothing to hang off.
    Uniform<Float4> color;

    EACP_SHADER(color)
};

struct BlendView final : GPUView
{
    explicit BlendView(BlendMode modeToUse)
        : mode(modeToUse)
    {
        setSampleCount(1);
        shader.color = Array {1.f, 0.f, 0.f, 0.5f};
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

    check(pixel.g > 0.9f);
    check(pixel.r > 0.2f);
};

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
