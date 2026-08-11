#include <eacp/GPUWidgets/GPUWidgets.h>

using namespace eacp;

namespace
{
constexpr auto background = Graphics::Color::gray(0.09f);
constexpr auto ink = Graphics::Color {0.98f, 0.72f, 0.24f};
constexpr auto headerHeight = 34.f;
constexpr auto strokeWidth = 9.f;
constexpr auto miterLimit = 4.f;

template <typename PathType>
PathType comparedShapes(const Graphics::Rect& panel)
{
    auto path = PathType {};

    auto left = panel.w * 0.12f;
    auto right = panel.w * 0.88f;
    auto span = right - left;

    auto zigTop = panel.h * 0.08f;
    auto zigBottom = panel.h * 0.24f;

    path.moveTo({left, zigBottom});

    for (auto i = 1; i <= 5; ++i)
        path.lineTo(
            {left + span * (float) i / 5.f, i % 2 == 0 ? zigBottom : zigTop});

    auto waveY = panel.h * 0.44f;
    auto waveHeight = panel.h * 0.13f;

    path.moveTo({left, waveY});
    path.cubicTo(left + span * 0.3f,
                 waveY - waveHeight,
                 left + span * 0.7f,
                 waveY + waveHeight,
                 right,
                 waveY);

    auto boxHeight = panel.h * 0.22f;
    path.addRoundedRect({left, panel.h * 0.66f, span, boxHeight}, boxHeight * 0.3f);

    return path;
}

Graphics::WindowOptions windowOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = 1122;
    options.height = 620;
    options.title = "eacp - Path Stroke";
    options.minWidth = 780;
    options.minHeight = 440;
    return options;
}
} // namespace

// Single-sampled: MSAA would smooth the quad's own edges, not the mask's.
struct StrokePanel final : GPU::GPUView
{
    StrokePanel()
    {
        setSampleCount(1);
        quad.prepareQuad(sampleCount());
    }

    void render(GPU::Frame& frame) override
    {
        rebuildIfNeeded();

        {
            auto compute = frame.beginCompute();
            rasterizer.dispatch(compute);
        }

        auto pass = frame.beginPass({background});

        if (rasterizer.isEmpty())
            return;

        auto covered = rasterizer.getCoveredBounds();
        const auto& mask = rasterizer.getCoverage();

        quad.setViewport(built.w * builtScale, built.h * builtScale);
        quad.drawMask(pass,
                      mask,
                      {covered.x * builtScale,
                       covered.y * builtScale,
                       (float) mask.width(),
                       (float) mask.height()},
                      ink);
    }

    void rebuildIfNeeded()
    {
        auto bounds = getLocalBounds();
        auto scale = backingScale();

        if (bounds.w == built.w && bounds.h == built.h && scale == builtScale)
            return;

        built = bounds;
        builtScale = scale;

        rasterizer.setScale(scale);
        rasterizer.setPath(build(bounds));
    }

    std::function<GPUWidgets::Path(const Graphics::Rect&)> build =
        [](const Graphics::Rect&) { return GPUWidgets::Path {}; };

    GPUWidgets::PathRasterizer rasterizer;
    GPUWidgets::CoverageShader quad;

    Graphics::Rect built;
    float builtScale = 0.f;
};

struct PlatformPanel final : Graphics::View
{
    void paint(Graphics::Context& g) override
    {
        auto bounds = getLocalBounds();

        g.setColor(background);
        g.fillRect(bounds);

        g.setColor(ink);
        g.setLineWidth(strokeWidth);
        g.strokePath(comparedShapes<Graphics::Path>(bounds));
    }

    void resized() override { repaint(); }
};

struct PathStrokeRoot final : Graphics::View
{
    PathStrokeRoot()
    {
        coverage.build = [](const Graphics::Rect& panel)
        {
            return GPUWidgets::strokeToFill(comparedShapes<GPUWidgets::Path>(panel),
                                            {strokeWidth,
                                             GPUWidgets::LineCap::Butt,
                                             GPUWidgets::LineJoin::Miter});
        };

        variants.build = [](const Graphics::Rect& panel) { return sampler(panel); };

        addChildren({coverage, platform, variants});
    }

    // Chevron angle kept inside the miter limit, so no join degrades to a bevel.
    static GPUWidgets::Path sampler(const Graphics::Rect& panel)
    {
        const GPUWidgets::LineJoin joins[] = {GPUWidgets::LineJoin::Miter,
                                              GPUWidgets::LineJoin::Round,
                                              GPUWidgets::LineJoin::Bevel};

        const GPUWidgets::LineCap caps[] = {GPUWidgets::LineCap::Butt,
                                            GPUWidgets::LineCap::Round,
                                            GPUWidgets::LineCap::Square};

        auto out = GPUWidgets::Path {};
        auto rowHeight = panel.h / 3.4f;
        auto left = panel.w * 0.16f;
        auto right = panel.w * 0.84f;

        for (auto row = 0; row < 3; ++row)
        {
            auto top = panel.h * 0.06f + (float) row * rowHeight;

            auto tipX = left + (right - left) * 0.55f;

            auto arrow = GPUWidgets::Path {};
            arrow.moveTo({left, top});
            arrow.lineTo({tipX, top + rowHeight * 0.35f});
            arrow.lineTo({left, top + rowHeight * 0.70f});

            auto stroked = GPUWidgets::strokeToFill(
                arrow, {strokeWidth, caps[row], joins[row], miterLimit});

            for (const auto& sub: stroked.getSubPaths())
            {
                auto first = true;

                for (const auto& point: sub.points)
                {
                    if (first)
                        out.moveTo(point);
                    else
                        out.lineTo(point);

                    first = false;
                }

                out.close();
            }
        }

        return out;
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        auto panel = bounds.w / 3.f;
        auto height = bounds.h - headerHeight;

        coverage.setBounds({0.f, headerHeight, panel, height});
        platform.setBounds({panel, headerHeight, panel, height});
        variants.setBounds({panel * 2.f, headerHeight, panel, height});
    }

    void paint(Graphics::Context& g) override
    {
        auto bounds = getLocalBounds();
        auto panel = bounds.w / 3.f;

        g.setColor(background);
        g.fillRect(bounds);

        g.setColor(Graphics::Color::gray(0.62f));

        const char* labels[] = {
            "compute coverage", "CoreGraphics", "joins and caps"};

        for (auto i = 0; i < 3; ++i)
            g.drawText(labels[i], {panel * (float) i + 18.f, 22.f}, font);
    }

    StrokePanel coverage;
    PlatformPanel platform;
    StrokePanel variants;

    Graphics::Font font {Graphics::FontOptions {}.withSize(13.f)};
};

struct PathStrokeApp
{
    PathStrokeApp() { window.setContentView(root); }

    PathStrokeRoot root;
    Graphics::Window window {windowOptions()};
};

int main()
{
    return eacp::Apps::run<PathStrokeApp>();
}
