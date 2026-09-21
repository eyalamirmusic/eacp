#include <eacp/GPUWidgets/GPUWidgets.h>

#include <cstdio>

using namespace eacp;
using namespace GPU;

// Analytic coverage, computed in a kernel: PathRasterizer walks a path's
// segments once per pixel and writes the exact fraction of that pixel the path
// covers, which CoverageShader then paints a colour through.
//
// The same self-intersecting star is rasterized twice, under each fill rule -
// non-zero on the left, where the winding never cancels and the middle is
// solid; even-odd on the right, where it does and the middle is a pentagonal
// hole. That is the fill rule doing something no amount of tessellation quality
// would show.
//
// The view is deliberately single-sampled: every smooth edge here is the
// kernel's arithmetic, with no multisampling anywhere in the pipeline to
// flatter it.

namespace
{
constexpr auto background = Graphics::Color::gray(0.09f);
constexpr auto fill = Graphics::Color {0.98f, 0.72f, 0.24f};

// A five-pointed star traced in one stroke, every second vertex - so the
// outline crosses itself and the two fill rules disagree about the middle.
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

        reportCrossing();
    }

    // What the frame paid to have its coverage computed on one backend and
    // painted on another (plan.md D11). Said on the first frame that crosses
    // anything and once every sixty after it, and not at all on a Device with
    // one backend - which is every Device but Linux's composite. This view
    // redraws only when it is resized, so the first line is usually the only
    // one.
    void reportCrossing()
    {
        auto& device = Device::shared();

        if (device.crossingBytesThisFrame() <= 0)
            return;

        crossedMilliseconds += device.crossingMillisecondsThisFrame();
        ++framesSinceReport;

        if (reported && framesSinceReport < 60)
            return;

        std::printf("coverage crossed %7.1f KB in %2d copies, %6.3f ms a "
                    "frame\n",
                    (double) device.crossingBytesThisFrame() / 1024.0,
                    device.crossingsThisFrame(),
                    crossedMilliseconds / (double) framesSinceReport);
        std::fflush(stdout);

        reported = true;
        framesSinceReport = 0;
        crossedMilliseconds = 0.0;
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

    // The path is authored in logical points and rasterized at the device
    // scale, so the coverage texture is one texel per physical pixel however
    // the window is sized or whichever display it is on.
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

    double crossedMilliseconds = 0.0;
    int framesSinceReport = 0;
    bool reported = false;
};

int main()
{
    return Graphics::runWindowedApp<PathCoverageView>(windowOptions());
}
