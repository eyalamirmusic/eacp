#include <eacp/GPUWidgets/GPUWidgets.h>

#include <chrono>
#include <cmath>
#include <cstdio>

using namespace eacp;
using Graphics::Point;
using Graphics::Rect;

// What a rasterization costs on each side of the wire.
//
// Rung two priced a path by its outline instead of by its area, and the numbers
// it reported were segment-pixel tests - the GPU's work. That was the right unit
// then, because the GPU's work was the whole of it. It is not any more: binning
// happens on the CPU, and every path that moves is re-binned, prefix-summed and
// re-uploaded before the frame it appears in.
//
// So this measures the two sides against each other rather than either alone.
// setPath is the CPU: emit the segments, bin them into tiles, sum the backdrops,
// upload three buffers. The dispatch is the GPU. A rung above this one moves
// work from the first column to the second, and it is only worth building if the
// first column is where the time is.
//
// Headless on purpose - no window, no swapchain, no compositor - so what is
// timed is the rasterizer and not a frame.

namespace
{
constexpr auto repetitions = 200;
constexpr auto warmups = 20;

using Clock = std::chrono::steady_clock;

double millisecondsSince(Clock::time_point start)
{
    auto elapsed = Clock::now() - start;
    return std::chrono::duration<double, std::milli>(elapsed).count();
}

// ------------------------------------------------------------------ the paths

Point onCircle(Point centre, float radius, float angle)
{
    return {centre.x + std::sin(angle) * radius,
            centre.y - std::cos(angle) * radius};
}

// UI::Knob's indicator, geometry for geometry: a ring segment as a closed
// contour, and the pointer as a second one wound the same way.
GPUWidgets::Path knobIndicator(float size, float value)
{
    constexpr auto startAngle = -2.356194f;
    constexpr auto sweepAngle = 4.712389f;

    auto centre = Point {size * 0.5f, size * 0.5f};
    auto outer = size * 0.5f - 1.f;
    auto thickness = std::max(2.f, size * 0.12f);
    auto inner = outer - thickness;
    auto sweep = sweepAngle * value;
    auto steps = std::max(8, (int) std::ceil(sweep * outer * 0.5f));

    auto path = GPUWidgets::Path {};
    path.moveTo(onCircle(centre, outer, startAngle));

    for (auto i = 1; i <= steps; ++i)
        path.lineTo(
            onCircle(centre, outer, startAngle + sweep * (float) i / (float) steps));

    for (auto i = steps; i >= 0; --i)
        path.lineTo(
            onCircle(centre, inner, startAngle + sweep * (float) i / (float) steps));

    path.close();

    auto pointerWidth = std::max(1.5f, size * 0.045f);
    auto angle = startAngle + sweep;
    auto tip = onCircle(centre, outer - thickness * 0.5f, angle);
    auto across =
        Point {std::cos(angle) * pointerWidth, std::sin(angle) * pointerWidth};

    path.moveTo({centre.x - across.x, centre.y - across.y});
    path.lineTo({tip.x - across.x, tip.y - across.y});
    path.lineTo({tip.x + across.x, tip.y + across.y});
    path.lineTo({centre.x + across.x, centre.y + across.y});
    path.close();

    return path;
}

// Apps/GPU/PathQuality's panel: a rounded rectangle and an ellipse, which
// between them cover the straight edges and the near-tangent curvature a UI
// actually asks for.
GPUWidgets::Path qualityPanel(const Rect& panel)
{
    auto rounded =
        Rect {panel.w * 0.08f, panel.h * 0.05f, panel.w * 0.84f, panel.h * 0.30f};
    auto size = std::min(panel.w * 0.72f, panel.h * 0.48f);
    auto ellipse = Rect {(panel.w - size) * 0.5f, panel.h * 0.44f, size, size};

    auto path = GPUWidgets::Path {};
    path.addRoundedRect(rounded, rounded.h * 0.34f);
    path.addEllipse(ellipse);
    return path;
}

// The case the whole rasterizer is aimed at: a wide, shallow, curve-heavy region
// of the kind a DAW draws under an automation lane. Long in x, thin in y, and
// nearly all outline - the shape for which area-priced rasterization was
// hopeless and outline-priced rasterization is ordinary.
GPUWidgets::Path automationCurve(float width, float height, int lobes)
{
    auto path = GPUWidgets::Path {};
    path.moveTo({0.f, height * 0.5f});

    auto span = width / (float) lobes;

    for (auto i = 0; i < lobes; ++i)
    {
        auto x = span * (float) i;
        auto rising = (i % 2) == 0;
        auto to = rising ? height * 0.08f : height * 0.92f;
        auto from = rising ? height * 0.92f : height * 0.08f;

        path.cubicTo(x + span * 0.35f, from, x + span * 0.65f, to, x + span, to);
    }

    // Closed back along the lane's floor, so it is a filled region rather than a
    // ribbon - which is what an automation lane draws.
    path.lineTo({width, height});
    path.lineTo({0.f, height});
    path.close();

    return path;
}

// An outline with a real segment count behind it: the map layer, the SVG
// document, the vector editor's artwork. Concentric rings rather than one
// enormous contour, because that is the shape those actually have - many
// contours over one area, which is also what makes them overlap.
GPUWidgets::Path denseArtwork(float extent, int rings, int sides)
{
    auto path = GPUWidgets::Path {};
    auto centre = Point {extent * 0.5f, extent * 0.5f};

    for (auto ring = 0; ring < rings; ++ring)
    {
        auto radius = extent * 0.48f * (float) (ring + 1) / (float) rings;

        path.moveTo(onCircle(centre, radius, 0.f));

        for (auto i = 1; i < sides; ++i)
        {
            constexpr auto tau = 6.2831853f;
            auto angle = tau * (float) i / (float) sides;

            // A wobble on the radius, so no two rings are the same polygon and
            // no segment is exactly horizontal.
            auto wobble = 1.f + 0.06f * std::sin(angle * 7.f + (float) ring);
            path.lineTo(onCircle(centre, radius * wobble, angle));
        }

        path.close();
    }

    return path;
}

GPUWidgets::Path fullWindowEllipse(float width, float height)
{
    auto path = GPUWidgets::Path {};
    path.addEllipse({0.f, 0.f, width, height});
    return path;
}

// ------------------------------------------------------------------ the timing

struct Case
{
    const char* name;
    GPUWidgets::Path path;
    float scale;
};

// setPath does the whole CPU side: emit, bin, prefix-sum the backdrops, upload.
// Timed with the buffers already grown, because that is the steady state - a
// path that moves every frame reallocates nothing after its first frame, and
// timing the first would be measuring the allocator.
double cpuMilliseconds(GPUWidgets::PathRasterizer& rasterizer,
                       const GPUWidgets::Path& path)
{
    for (auto i = 0; i < warmups; ++i)
        rasterizer.setPath(path);

    auto start = Clock::now();

    for (auto i = 0; i < repetitions; ++i)
        rasterizer.setPath(path);

    return millisecondsSince(start) / (double) repetitions;
}

// One command buffer submitted and waited on with nothing in it. Every GPU
// figure below rides on top of this, and at UI scale it is most of what a naive
// reading would call the GPU's time - so it is measured and subtracted rather
// than left to flatter the comparison.
double submitFloor()
{
    auto once = []
    {
        auto commands = GPU::Device::shared().makeCommandBuffer();

        {
            auto pass = commands.beginCompute();
            (void) pass;
        }

        commands.commit();
    };

    for (auto i = 0; i < 8; ++i)
        once();

    constexpr auto passes = 40;
    auto start = Clock::now();

    for (auto i = 0; i < passes; ++i)
        once();

    return millisecondsSince(start) / (double) passes;
}

// Many dispatches on one command buffer, committed once. Submitting each one on
// its own and waiting measures the round trip instead of the kernel - at UI
// scale that is several times the work itself, and it made a 40pt knob look
// dearer than a 96pt one. Batching is also what a frame does.
double gpuMilliseconds(GPUWidgets::PathRasterizer& rasterizer, double floor)
{
    constexpr auto passes = 20;

    auto batch = [&]
    {
        auto commands = GPU::Device::shared().makeCommandBuffer();

        {
            auto pass = commands.beginCompute();

            for (auto i = 0; i < passes; ++i)
                rasterizer.dispatch(pass);
        }

        commands.commit();
    };

    for (auto i = 0; i < 2; ++i)
        batch();

    constexpr auto rounds = 5;
    auto start = Clock::now();

    for (auto i = 0; i < rounds; ++i)
        batch();

    auto perBatch = millisecondsSince(start) / (double) rounds;
    return std::max(0.0, (perBatch - floor) / (double) passes);
}

void report(Case& item, double floor)
{
    auto rasterizer = GPUWidgets::PathRasterizer {};
    rasterizer.setScale(item.scale);
    rasterizer.setPath(item.path);

    if (rasterizer.isEmpty())
    {
        std::printf("%-26s  (empty)\n", item.name);
        return;
    }

    auto cpu = cpuMilliseconds(rasterizer, item.path);
    auto gpu = gpuMilliseconds(rasterizer, floor);

    std::printf("%-26s %5d x %-5d %8d %12lld %9.3f %9.3f %8.0f%%\n",
                item.name,
                rasterizer.getCoverageWidth(),
                rasterizer.getCoverageHeight(),
                rasterizer.getSegmentCount(),
                rasterizer.getSegmentTests(),
                cpu,
                gpu,
                100.0 * cpu / (cpu + gpu));
}

// The workload rung 3 is actually for: a canvas of paths, all of them moving, so
// every one is re-binned and re-uploaded every frame. One rasterizer each, the
// way a tree of widgets holds one PathShape each, and the whole set timed
// together - because what a frame has to fit in is the sum.
void reportCanvas(const char* name,
                  int count,
                  const GPUWidgets::Path& path,
                  float scale)
{
    auto rasterizers = Vector<GPUWidgets::PathRasterizer> {};
    rasterizers.resize(count);

    for (auto& rasterizer: rasterizers)
    {
        rasterizer.setScale(scale);
        rasterizer.setPath(path);
    }

    auto cpuOnce = [&]
    {
        for (auto& rasterizer: rasterizers)
            rasterizer.setPath(path);
    };

    auto gpuOnce = [&]
    {
        auto commands = GPU::Device::shared().makeCommandBuffer();

        {
            auto pass = commands.beginCompute();

            for (auto& rasterizer: rasterizers)
                rasterizer.dispatch(pass);
        }

        commands.commit();
    };

    for (auto i = 0; i < 4; ++i)
    {
        cpuOnce();
        gpuOnce();
    }

    constexpr auto rounds = 20;

    auto cpuStart = Clock::now();

    for (auto i = 0; i < rounds; ++i)
        cpuOnce();

    auto cpu = millisecondsSince(cpuStart) / (double) rounds;

    auto gpuStart = Clock::now();

    for (auto i = 0; i < rounds; ++i)
        gpuOnce();

    auto gpu = millisecondsSince(gpuStart) / (double) rounds;

    std::printf("%-26s %13d %8d %12s %9.3f %9.3f %8.0f%%\n",
                name,
                count,
                rasterizers[0].getSegmentCount() * count,
                "-",
                cpu,
                gpu,
                100.0 * cpu / (cpu + gpu));
}
} // namespace

int main()
{
    if (!GPU::Device::shared().isValid())
    {
        std::printf("no GPU device\n");
        return 1;
    }

    auto floor = submitFloor();

    auto cases = Vector<Case> {};
    cases.add({"knob indicator, 40pt", knobIndicator(40.f, 0.72f), 2.f});
    cases.add({"knob indicator, 96pt", knobIndicator(96.f, 0.72f), 2.f});
    cases.add({"PathQuality panel", qualityPanel({0.f, 0.f, 304.f, 497.f}), 2.f});
    cases.add({"automation curve, 1200pt", automationCurve(1200.f, 192.f, 40), 2.f});
    cases.add({"full-window ellipse", fullWindowEllipse(1600.f, 1000.f), 2.f});
    cases.add({"artwork, 4k segments", denseArtwork(900.f, 40, 100), 2.f});
    cases.add({"artwork, 20k segments", denseArtwork(900.f, 100, 200), 2.f});
    cases.add({"artwork, 100k segments", denseArtwork(900.f, 200, 500), 2.f});

    std::printf("submit floor: %.3f ms\n\n", floor);

    std::printf("%-26s %13s %8s %12s %9s %9s %9s\n",
                "path",
                "coverage px",
                "segments",
                "seg. tests",
                "CPU ms",
                "GPU ms",
                "CPU share");

    for (auto& item: cases)
        report(item, floor);

    std::printf("\n%-26s %13s %8s %12s %9s %9s %9s\n",
                "canvas, all moving",
                "paths",
                "segments",
                "",
                "CPU ms",
                "GPU ms",
                "CPU share");

    auto lane = automationCurve(1200.f, 192.f, 40);
    reportCanvas("automation lanes x 32", 32, lane, 2.f);
    reportCanvas("automation lanes x 128", 128, lane, 2.f);

    auto shape = qualityPanel({0.f, 0.f, 304.f, 497.f});
    reportCanvas("panels x 128", 128, shape, 2.f);

    return 0;
}
