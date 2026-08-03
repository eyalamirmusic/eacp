#include <eacp/GPUWidgets/GPUWidgets.h>

using namespace eacp;

// Stroking through the coverage kernel, next to the platform's own stroker.
//
//   left    strokeToFill + PathRasterizer - the stroke turned into a fill and
//           rasterized single-sampled, so nothing but the arithmetic is
//           smoothing those edges
//   middle  Graphics::Context::strokePath - CoreGraphics here, Direct2D on
//           Windows, the standard to beat
//   right   the joins and caps, which the middle panel cannot draw: the
//           platform Context takes a line width and nothing else, so there is
//           no honest way to put them side by side
//
// The two compared panels stroke identical geometry with a miter join and a
// butt cap, which is what the platform does when it is not told otherwise.

namespace
{
constexpr auto background = Graphics::Color::gray(0.09f);
constexpr auto ink = Graphics::Color {0.98f, 0.72f, 0.24f};
constexpr auto headerHeight = 34.f;
constexpr auto strokeWidth = 9.f;

// Sharp corners, a smooth curve, and a closed contour - between them every case
// a join has to handle, and the curve is where a stroker that leaves gaps
// between its segment quads gives itself away as a row of spokes.
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

// Rasterizes whatever path it is given and paints the mask. Single-sampled on
// purpose: MSAA here would be smoothing the quad's own axis-aligned edges while
// taking credit for the mask's.
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

    // Set by the owner: what this panel strokes.
    std::function<GPUWidgets::Path(const Graphics::Rect&)> build =
        [](const Graphics::Rect&) { return GPUWidgets::Path {}; };

    GPUWidgets::PathRasterizer rasterizer;
    GPUWidgets::CoverageShader quad;

    Graphics::Rect built;
    float builtScale = 0.f;
};

// The platform's panel, stroking the same geometry through the same Context
// every non-GPU eacp view draws with.
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

    // Every join against every cap, on a chevron of about fifty degrees - open
    // enough that a miter is still inside the limit, so the three joins come out
    // as three plainly different corners rather than as three bevels.
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
                arrow, {strokeWidth, caps[row], joins[row], 4.f});

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
