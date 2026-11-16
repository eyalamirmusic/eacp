#include <eacp/Core/Utils/Environment.h>
#include <eacp/GPU/GPU.h>
#include <eacp/Graphics/Graphics.h>

#include <cstdlib>
#include <string>

// GPUView::setRenderScale: the view renders fewer pixels than it covers and the
// window system stretches them over it. The pattern is fine rings, a few pixels
// apart at full resolution, so a lower scale shows at once as blocky, aliased
// rings while the view's size, layout and input stay the same.
//
// Click, tap, or press 1-4 to pick 1, 0.75, 0.5 or 0.25; EACP_RENDER_SCALE=0.5
// starts at that scale. The first frame at each scale logs its size, so the log
// says what the platform really built: a smaller drawable where the compositor stretches
// (macOS, iOS, Windows, Android, Wayland with a viewporter), the full one on X11.

using namespace eacp;
using namespace GPU;
using namespace Maths;

namespace
{
constexpr float scales[] = {1.f, 0.75f, 0.5f, 0.25f};
constexpr auto scaleCount = static_cast<int>(std::size(scales));
constexpr auto ringsAcrossTheViewCount = 60.f;

struct QuadVertex
{
    Vec2 position;
};

const QuadVertex fullScreenQuad[] = {
    {{-1.f, -1.f}},
    {{1.f, -1.f}},
    {{-1.f, 1.f}},
    {{1.f, -1.f}},
    {{1.f, 1.f}},
    {{-1.f, 1.f}},
};

// Rings around the view's centre, alternating light and dark: the thinnest
// detail a frame can hold, which is what a lower render scale loses first.
struct RingsShader final : ShaderProgram
{
    RingsShader()
    {
        compile();
        ringsAcrossTheView = ringsAcrossTheViewCount;
    }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        auto fromCentre = varying(position * 0.5f);

        setPosition(float4(position, 0.f, 1.f));

        auto ring = step(0.5f, fract(length(fromCentre) * ringsAcrossTheView));
        auto dark = float4(constant(0.1f), 0.12f, 0.16f, 1.f);
        auto light = float4(constant(0.95f), 0.85f, 0.55f, 1.f);
        setFragment(mix(dark, light, ring));
    }

    Uniform<Float> ringsAcrossTheView;

    EACP_SHADER(ringsAcrossTheView)
};

int getScaleIndexFromEnvironment()
{
    const auto value = getEnvValue("EACP_RENDER_SCALE");

    if (value.empty())
        return 0;

    const auto asked = std::strtof(value.c_str(), nullptr);

    for (auto index = 0; index < scaleCount; ++index)
        if (asked >= scales[index])
            return index;

    return scaleCount - 1;
}

int getScaleIndexForKey(const std::string& characters)
{
    for (auto index = 0; index < scaleCount; ++index)
        if (characters == std::to_string(index + 1))
            return index;

    return -1;
}
} // namespace

struct RenderScaleView final : GPUView
{
    RenderScaleView()
    {
        setHandlesTouchEvents(true);
        shader.setVertices(fullScreenQuad);
        shader.prepare(sampleCount());
        onBackingScaleChanged = [this](float) { shouldLogFrameSize = true; };
        applyScale(getScaleIndexFromEnvironment());
    }

    void render(Frame& frame) override
    {
        if (shouldLogFrameSize)
            logFrameSize(frame);

        auto pass = frame.beginPass({});
        pass.draw(shader);
    }

    void mouseDown(const Graphics::MouseEvent&) override { applyNextScale(); }

    void touchEnded(const Graphics::TouchEvent&) override { applyNextScale(); }

    void keyDown(const Graphics::KeyEvent& key) override
    {
        const auto index = getScaleIndexForKey(key.characters);

        if (index >= 0)
            applyScale(index);
    }

private:
    void applyNextScale() { applyScale((scaleIndex + 1) % scaleCount); }

    void applyScale(int index)
    {
        scaleIndex = index;
        setRenderScale(scales[index]);
        shouldLogFrameSize = true;
        LOG("RenderScale: asked for ", scales[index]);
    }

    void logFrameSize(const Frame& frame)
    {
        shouldLogFrameSize = false;
        const auto pixels = frame.pixelSize();
        const auto points = frame.logicalSize();
        const auto pixelWidth = static_cast<int>(pixels.x);
        const auto pixelHeight = static_cast<int>(pixels.y);
        const auto pointWidth = static_cast<int>(points.x);
        const auto pointHeight = static_cast<int>(points.y);

        LOG("RenderScale: render scale ",
            renderScale(),
            ", frame ",
            pixelWidth,
            "x",
            pixelHeight,
            " pixels over ",
            pointWidth,
            "x",
            pointHeight,
            " points (backing scale ",
            backingScale(),
            ")");
    }

    RingsShader shader;
    int scaleIndex = 0;
    bool shouldLogFrameSize = true;
};

int main()
{
    return Graphics::runWindowedApp<RenderScaleView>();
}
