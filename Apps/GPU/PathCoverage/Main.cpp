#include <eacp/GPUWidgets/GPUWidgets.h>

using namespace eacp;
using namespace GPU;

namespace
{
constexpr auto background = Graphics::Color::gray(0.09f);
constexpr auto fill = Graphics::Color {0.98f, 0.72f, 0.24f};

// Every second vertex, so the outline crosses itself and the two fill rules
// disagree about the middle.
GPUWidgets::Path makeStar(const Graphics::Point& centre, float radius)
{
    constexpr auto points = 5;
    auto path = GPUWidgets::Path {};

    for (auto i = 0; i < points; ++i)
    {
        auto turn = (float) (i * 2 % points) / (float) points;
        auto angle = turn * 2.f * GPUWidgets::pi - GPUWidgets::pi * 0.5f;

        auto vertex = Graphics::Point {centre.x + std::cos(angle) * radius,
                                       centre.y + std::sin(angle) * radius};

        if (i == 0)
            path.moveTo(vertex);
        else
            path.lineTo(vertex);
    }

    path.close();
    return path;
}

Graphics::WindowOptions windowOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = 900;
    options.height = 500;
    options.title = "eacp - Path Coverage";
    options.minWidth = 320;
    options.minHeight = 240;
    return options;
}
} // namespace

struct PathCoverageView final : GPUView
{
    PathCoverageView()
    {
        // Single-sampled: every smooth edge here is the kernel's arithmetic,
        // with no multisampling in the pipeline to flatter it.
        setSampleCount(1);
        quad.prepareQuad(sampleCount());
    }

    void render(Frame& frame) override
    {
        rasterizeIfNeeded();

        {
            auto compute = frame.beginCompute();
            nonZero.dispatch(compute);
            evenOdd.dispatch(compute);
        }

        auto pass = frame.beginPass({background});

        quad.setViewport(builtWidth * builtScale, builtHeight * builtScale);

        drawCoverage(pass, nonZero);
        drawCoverage(pass, evenOdd);
    }

    void drawCoverage(RenderPass& pass, const GPUWidgets::PathRasterizer& rasterizer)
    {
        if (rasterizer.isEmpty())
            return;

        const auto& mask = rasterizer.getCoverage();
        auto covered = rasterizer.getCoveredBounds();

        quad.drawMask(pass,
                      mask,
                      {covered.x * builtScale,
                       covered.y * builtScale,
                       (float) mask.width(),
                       (float) mask.height()},
                      fill);
    }

    // Authored in logical points but rasterized at the device scale, so the
    // coverage texture is one texel per physical pixel.
    void rasterizeIfNeeded()
    {
        auto bounds = getLocalBounds();
        auto scale = backingScale();

        if (bounds.w == builtWidth && bounds.h == builtHeight && scale == builtScale)
            return;

        builtWidth = bounds.w;
        builtHeight = bounds.h;
        builtScale = scale;

        auto panel = builtWidth * 0.5f;
        auto radius = std::min(panel, builtHeight) * 0.38f;

        nonZero.setScale(scale);
        evenOdd.setScale(scale);

        nonZero.setPath(makeStar({panel * 0.5f, builtHeight * 0.5f}, radius),
                        GPUWidgets::FillRule::NonZero);

        evenOdd.setPath(makeStar({panel * 1.5f, builtHeight * 0.5f}, radius),
                        GPUWidgets::FillRule::EvenOdd);
    }

    GPUWidgets::PathRasterizer nonZero;
    GPUWidgets::PathRasterizer evenOdd;
    GPUWidgets::CoverageShader quad;

    float builtWidth = 0.f;
    float builtHeight = 0.f;
    float builtScale = 0.f;
};

struct PathCoverageApp
{
    PathCoverageApp() { window.setContentView(view); }

    PathCoverageView view;
    Graphics::Window window {windowOptions()};
};

int main()
{
    return eacp::Apps::run<PathCoverageApp>();
}
