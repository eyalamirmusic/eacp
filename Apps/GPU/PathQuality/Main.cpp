#include <eacp/GPUWidgets/GPUWidgets.h>

using namespace eacp;

namespace
{
constexpr auto background = Graphics::Color::gray(0.09f);
constexpr auto fill = Graphics::Color {0.98f, 0.72f, 0.24f};
constexpr auto headerHeight = 34.f;

Graphics::Rect roundedRectFor(const Graphics::Rect& panel)
{
    return {panel.w * 0.08f, panel.h * 0.05f, panel.w * 0.84f, panel.h * 0.30f};
}

Graphics::Rect ellipseFor(const Graphics::Rect& panel)
{
    auto size = std::min(panel.w * 0.72f, panel.h * 0.48f);
    return {(panel.w - size) * 0.5f, panel.h * 0.44f, size, size};
}

template <typename PathType>
PathType makeShapes(const Graphics::Rect& panel)
{
    auto rounded = roundedRectFor(panel);

    auto path = PathType {};
    path.addRoundedRect(rounded, rounded.h * 0.34f);
    path.addEllipse(ellipseFor(panel));
    return path;
}

Graphics::WindowOptions windowOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = 1080;
    options.height = 640;
    options.title = "eacp - Path Quality";
    options.minWidth = 640;
    options.minHeight = 400;
    return options;
}
} // namespace

// Single-sampled: MSAA would smooth the quad's own edges, not the mask's.
struct CoveragePanel final : GPU::GPUView
{
    CoveragePanel()
    {
        setSampleCount(1);
        quad.prepareQuad(sampleCount());
    }

    void render(GPU::Frame& frame) override
    {
        rasterizeIfNeeded();

        {
            auto compute = frame.beginCompute();
            rasterizer.dispatch(compute);
        }

        auto pass = frame.beginPass({background});

        if (rasterizer.isEmpty())
            return;

        const auto& mask = rasterizer.getCoverage();
        auto covered = rasterizer.getCoveredBounds();

        quad.setViewport(built.w * builtScale, built.h * builtScale);
        quad.drawMask(pass,
                      mask,
                      {covered.x * builtScale,
                       covered.y * builtScale,
                       (float) mask.width(),
                       (float) mask.height()},
                      fill);
    }

    void rasterizeIfNeeded()
    {
        auto bounds = getLocalBounds();
        auto scale = backingScale();

        if (bounds.w == built.w && bounds.h == built.h && scale == builtScale)
            return;

        built = bounds;
        builtScale = scale;

        rasterizer.setScale(scale);
        rasterizer.setPath(makeShapes<GPUWidgets::Path>(bounds));
    }

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

        g.setColor(fill);
        g.fillPath(makeShapes<Graphics::Path>(bounds));
    }

    void resized() override { repaint(); }
};

struct PathQualityRoot final : Graphics::View
{
    PathQualityRoot()
    {
        tessellated.setFillColor(fill);
        tessellated.setBackgroundColor(background);

        addChildren({coverage, tessellated, platform});
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        auto panel = bounds.w / 3.f;
        auto height = bounds.h - headerHeight;

        coverage.setBounds({0.f, headerHeight, panel, height});
        tessellated.setBounds({panel, headerHeight, panel, height});
        platform.setBounds({panel * 2.f, headerHeight, panel, height});

        tessellated.setPath(makeShapes<GPUWidgets::Path>({0.f, 0.f, panel, height}));
    }

    void paint(Graphics::Context& g) override
    {
        auto bounds = getLocalBounds();
        auto panel = bounds.w / 3.f;

        g.setColor(background);
        g.fillRect(bounds);

        g.setColor(Graphics::Color::gray(0.62f));

        const char* labels[] = {
            "compute coverage", "ear clip + 4x MSAA", "CoreGraphics"};

        for (auto i = 0; i < 3; ++i)
            g.drawText(labels[i], {panel * (float) i + 18.f, 22.f}, font);
    }

    CoveragePanel coverage;
    GPUWidgets::PathView tessellated;
    PlatformPanel platform;

    Graphics::Font font {Graphics::FontOptions {}.withSize(13.f)};
};

struct PathQualityApp
{
    PathQualityApp() { window.setContentView(root); }

    PathQualityRoot root;
    Graphics::Window window {windowOptions()};
};

int main()
{
    return eacp::Apps::run<PathQualityApp>();
}
