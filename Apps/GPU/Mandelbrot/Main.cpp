#include <eacp/Core/App/Args.h>
#include <eacp/GPU/GPU.h>

#include <cmath>

using namespace eacp;
using namespace GPU;

// The zoom falls into the Feigenbaum point on the real axis, which is
// self-similar under a scale of delta squared, so one cycle loops seamlessly
// and the framing stays symmetric about the real axis while abs(x) mirrors it
// left to right.

namespace
{
constexpr auto feigenbaumPointReal = -1.40115518909f;
constexpr auto feigenbaumDelta = 4.66920160910f;
constexpr auto zoomCycleScaleRatio = feigenbaumDelta * feigenbaumDelta;
constexpr auto zoomCycleStartHalfHeight = 0.05f;
constexpr auto zoomCycleSeconds = 24.f;

constexpr auto baseIterationCount = 120.f;
constexpr auto iterationsPerZoomDoubling = 40.f;
constexpr auto maximumIterationCount = 1000.f;
constexpr auto escapeRadiusSquared = 256.f * 256.f;

constexpr auto breathingRadiansPerSecond = 1.3f;
constexpr auto breathingDepth = 0.25f;
constexpr auto bandFrequency = 0.11f;
constexpr auto bandDriftPerSecond = 0.6f;
constexpr auto vignetteInnerRadius = 0.35f;
constexpr auto vignetteOuterRadius = 1.45f;

constexpr auto windowSize = 800;
constexpr auto windowMinimumSize = 320;
constexpr auto snapshotSize = 800.f;

struct QuadVertex
{
    float position[2];
};
} // namespace

EACP_SHADER_VALUE(QuadVertex, Float2)

namespace
{
constexpr QuadVertex fullQuad[] = {
    {{-1.f, -1.f}},
    {{+1.f, -1.f}},
    {{-1.f, +1.f}},
    {{+1.f, -1.f}},
    {{+1.f, +1.f}},
    {{-1.f, +1.f}},
};

float calculateZoomHalfHeight(float seconds)
{
    auto cyclePhase = std::fmod(seconds, zoomCycleSeconds) / zoomCycleSeconds;
    return zoomCycleStartHalfHeight * std::pow(zoomCycleScaleRatio, -cyclePhase);
}

float calculateIterationCount(float halfHeight)
{
    auto zoomDoublings = std::log2(zoomCycleStartHalfHeight / halfHeight);
    auto doublingsFromBase = std::log2(1.f / zoomCycleStartHalfHeight);
    auto count = baseIterationCount
                 + iterationsPerZoomDoubling * (zoomDoublings + doublingsFromBase);
    return std::min(count, maximumIterationCount);
}

struct DrawMirroredMandelbrot final : ShaderProgram
{
    DrawMirroredMandelbrot() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        auto screen = varying(position);

        setPosition(float4(position, 0.f, 1.f));

        auto mirroredX = abs(screen.x()) * aspectRatio;
        auto c = float2(mirroredX * halfHeight + feigenbaumPointReal,
                        screen.y() * halfHeight);

        auto z = var(float2(constant(0.f), constant(0.f)));
        auto iteration = var(0.f);
        auto escaped = var(false);

        loop(iteration < iterationCount,
             [&]
             {
                 auto current = z.get();
                 auto next = float2(current.x() * current.x()
                                        - current.y() * current.y() + c.x(),
                                    2.f * current.x() * current.y() + c.y());
                 z = next;

                 ifThen(dot(next, next) > escapeRadiusSquared,
                        [&]
                        {
                            escaped = true;
                            breakLoop();
                        });

                 iteration += 1.f;
             });

        auto magnitudeSquared = dot(z.get(), z.get());
        auto smoothIteration =
            iteration.get() + 1.f - log2(log2(magnitudeSquared) * 0.5f);

        auto breathing = 1.f - breathingDepth
                         + breathingDepth * sin(time * breathingRadiansPerSecond);
        auto band =
            0.5f
            + 0.5f
                  * sin(smoothIteration * bandFrequency - time * bandDriftPerSecond);
        auto heat = pow(band, 3.f) * breathing;

        auto black = float3(constant(0.f), constant(0.f), constant(0.f));
        auto bloodRed = float3(constant(0.35f), constant(0.f), constant(0.01f));
        auto crimson = float3(constant(1.f), constant(0.06f), constant(0.08f));

        auto glow = mix(mix(black, bloodRed, smoothstep(0.f, 0.4f, heat)),
                        crimson,
                        smoothstep(0.4f, 1.f, heat));
        auto colour = select(escaped.get(), glow, black);

        auto vignette =
            1.f
            - smoothstep(vignetteInnerRadius, vignetteOuterRadius, length(screen));

        setFragment(float4(colour * vignette, 1.f));
    }

    Uniform<Float> time;
    Uniform<Float> aspectRatio;
    Uniform<Float> halfHeight;
    Uniform<Float> iterationCount;

    EACP_SHADER(time, aspectRatio, halfHeight, iterationCount)
};

Graphics::WindowOptions createWindowOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = windowSize;
    options.height = windowSize;
    options.title = "eacp - Mandelbrot";
    options.minWidth = windowMinimumSize;
    options.minHeight = windowMinimumSize;
    return options;
}
} // namespace

struct MandelbrotView final : GPUView
{
    explicit MandelbrotView(float startSeconds = 0.f)
        : elapsedSeconds(startSeconds)
    {
        renderer.setVertices(fullQuad);
        renderer.prepare(sampleCount());
        setContinuous(true);
    }

    void update(Threads::FrameTime frameTime) override
    {
        elapsedSeconds += (float) frameTime.delta;
    }

    void render(Frame& frame) override
    {
        auto size = frame.logicalSize();
        auto zoomHalfHeight = calculateZoomHalfHeight(elapsedSeconds);

        renderer.time = elapsedSeconds;
        renderer.aspectRatio = size.x / std::max(size.y, 1.f);
        renderer.halfHeight = zoomHalfHeight;
        renderer.iterationCount = calculateIterationCount(zoomHalfHeight);

        auto pass = frame.beginPass({Graphics::Color {0.f, 0.f, 0.f}});
        pass.draw(renderer);
    }

    DrawMirroredMandelbrot renderer;
    float elapsedSeconds = 0.f;
};

namespace
{
int renderSnapshot(const FilePath& path, float seconds)
{
    auto view = MandelbrotView {seconds};
    view.setBounds({0.f, 0.f, snapshotSize, snapshotSize});

    auto image = view.renderToImage(1.f);

    if (!image.isValid())
        return 1;

    image.save(path);
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    auto snapshotPath = FilePath {};
    auto seconds = 0.f;

    auto cli = Args::Parser {"Mandelbrot", "A mirrored, red Mandelbrot zoom."};
    cli.positional(snapshotPath,
                   "snapshot",
                   "render one frame to this PNG and exit, with no window");
    cli.positional(seconds, "seconds", "the animation time of the snapshot");

    if (auto code = cli.exitCodeFor(cli.parse(argc, argv)))
        return *code;

    if (!snapshotPath.empty())
        return renderSnapshot(snapshotPath, seconds);

    return Graphics::runWindowedApp<MandelbrotView>(createWindowOptions());
}
