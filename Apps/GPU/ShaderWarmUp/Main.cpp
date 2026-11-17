#include <eacp/Core/App/Args.h>
#include <eacp/Core/Utils/Environment.h>
#include <eacp/UI/UI.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>

// Shaders compiled on a worker thread before the view that draws with them
// exists. The window opens on a progress screen - a ComponentHost reading
// GPU::ShaderWarmUp::progress() from a timer - while the warm-up compiles twelve
// raymarched scenes into the device's ShaderLibraryCache. When it resolves, the
// screen is swapped for a GPUView whose programs prepare against that cache, so
// building them compiles nothing. The scenes are added with their pipeline, so
// on Metal the GPU's own compile - which runs when a pipeline is made, about
// half as long again as the library - is done on the worker too, and the swap
// takes milliseconds rather than seconds of a frozen window. Elsewhere the
// pipelines are still made on the main thread, in the swap.
//
// Each scene's distance function is unrolled at record time, one term per
// shape, and evaluated eleven times per pixel (the march, four normal taps, a
// soft shadow, four occlusion taps and the colour), all inlined, so the source
// and the compile grow with --complexity, the shapes per scene. A per-launch
// salt is baked into every source so the system's own compile cache misses;
// --reuse-system-cache leaves it out to show a warm second launch.
//
// The progress screen's own shaders compile on the main thread before it can
// appear, which is the one compile a warm-up cannot hide. They are added to the
// warm-up as well, to show them coming back as already cached.
//
// --delay-ms (or EACP_SHADER_WARMUP_DELAY_MS) holds every shader in flight that
// much longer. `ShaderWarmUp out.png` warms up, draws one frame into a PNG and
// exits, with no window. --measure-responsiveness resizes the window on a timer
// and logs how late the main thread ran, during the warm-up and after it.

using namespace eacp;

namespace ShaderWarmUpDemo
{
constexpr auto tileColumns = 4;
constexpr auto tileRows = 3;
constexpr auto sceneCount = tileColumns * tileRows;
constexpr auto tileGap = 0.03f;
constexpr auto marchSteps = 80;
constexpr auto shadowSteps = 24;
constexpr auto farPlane = 18.f;
constexpr auto floorHeight = -1.3f;

using Clock = std::chrono::steady_clock;

double millisecondsSince(Clock::time_point started)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - started).count();
}

// FXC's compile time climbs far faster than linearly with the shape count:
// about 0.3 s a scene at 4 shapes, 3-4 s at 6 and minutes at 24, where Metal
// and glslang take 0.35-0.45 s. Both defaults land the warm-up at 4-6 s.
int defaultComplexity()
{
    return Platform::isWindows() ? 4 : 24;
}

struct Settings
{
    int complexity = defaultComplexity();
    float salt = 0.f;
};

struct Corner
{
    float position[2];
};

constexpr Corner unitQuad[] = {
    {{0.f, 0.f}},
    {{1.f, 0.f}},
    {{0.f, 1.f}},
    {{1.f, 0.f}},
    {{1.f, 1.f}},
    {{0.f, 1.f}},
};

enum class ShapeKind
{
    sphere,
    roundedBox,
    torus,
    octahedron
};

struct Shape
{
    ShapeKind kind = ShapeKind::sphere;
    float centre[3] {};
    float size = 0.2f;
    float orbit = 0.1f;
    float speed = 1.f;
    float phase = 0.f;
    float colour[3] {};
};

struct Random
{
    float next()
    {
        state = state * 1664525u + 1013904223u;
        return (float) (state >> 8) / 16777216.f;
    }

    float between(float low, float high) { return low + (high - low) * next(); }

    std::uint32_t state;
};

void paletteColour(int scene, float position, float* rgb)
{
    constexpr auto twoPi = 6.2831853f;
    const auto offset = (float) scene / (float) sceneCount;
    const float spread[3] = {0.f, 0.33f, 0.67f};

    for (auto channel = 0; channel < 3; ++channel)
        rgb[channel] =
            0.55f
            + 0.45f * std::cos(twoPi * (position * 0.8f + offset + spread[channel]));
}

void placeShape(int scene, int index, int count, Random& random, float* centre)
{
    constexpr auto twoPi = 6.2831853f;
    const auto along = (float) index / (float) count;

    switch (scene % 4)
    {
        case 0:
            centre[0] = 1.4f * std::cos(along * twoPi);
            centre[1] = random.between(-0.5f, 0.6f);
            centre[2] = 1.4f * std::sin(along * twoPi);
            break;
        case 1:
            centre[0] = std::cos((float) index * 0.9f) * 0.9f;
            centre[1] = -0.9f + 2.f * along;
            centre[2] = std::sin((float) index * 0.9f) * 0.9f;
            break;
        case 2:
            centre[0] = random.between(-1.2f, 1.2f);
            centre[1] = random.between(-0.8f, 0.9f);
            centre[2] = random.between(-1.2f, 1.2f);
            break;
        default:
            centre[0] = -1.7f + 3.4f * along;
            centre[1] = 0.5f * std::sin(along * twoPi * 2.f);
            centre[2] = random.between(-0.6f, 0.6f);
            break;
    }
}

Vector<Shape> shapesFor(int scene, int count)
{
    auto random = Random {0x9e3779b9u * (std::uint32_t) (scene + 1)};
    auto shapes = Vector<Shape> {};
    const auto scale = std::sqrt(24.f / (float) count);

    for (auto index = 0; index < count; ++index)
    {
        auto shape = Shape {};
        shape.kind = (ShapeKind) ((index + scene) % 4);
        placeShape(scene, index, count, random, shape.centre);
        shape.size = random.between(0.16f, 0.3f) * std::min(scale, 1.4f);
        shape.orbit = random.between(0.05f, 0.25f);
        shape.speed = random.between(0.4f, 1.3f);
        shape.phase = random.between(0.f, 6.28f);
        paletteColour(scene, (float) index / (float) count, shape.colour);
        shapes.add(shape);
    }

    return shapes;
}

std::string nameFor(int scene, const Settings& settings)
{
    return Strings::concat(
        "scene ", scene + 1, ", ", settings.complexity, " shapes");
}

// A raymarched scene of shapes blended into one surface over a floor. Every
// shape is a term written out by the C++ below, so the distance function grows
// with the shape count and is inlined at each of its eleven uses per pixel.
// Everything moves with `time`, so nothing folds away.
struct SceneShader final : GPU::ShaderProgram
{
    SceneShader(int sceneToUse, const Settings& settingsToUse)
        : scene(sceneToUse)
        , settings(settingsToUse)
        , shapes(shapesFor(sceneToUse, settingsToUse.complexity))
    {
        const auto started = Clock::now();
        compile();
        const auto total = millisecondsSince(started);

        LOG("ShaderWarmUp: ",
            nameFor(scene, settings),
            " recorded in ",
            (int) recordMilliseconds,
            " ms, emitted in ",
            (int) (total - recordMilliseconds),
            " ms, ",
            source().source.size() / 1024,
            " KB of source");
    }

    void define() override
    {
        const auto started = Clock::now();

        auto corner = vertexInput(&Corner::position);
        auto placed =
            float2(area.x(), area.y()) + corner * float2(area.z(), area.w());
        setPosition(float4(placed, 0.f, 1.f));

        auto uv = varying(corner);
        auto screen = float2((uv.x() * 2.f - 1.f) * aspect, uv.y() * 2.f - 1.f);

        auto angle = time * 0.15f + (float) scene * 0.7f;
        auto eye = float3(
            sin(angle) * 4.6f, 1.2f + 0.3f * sin(time * 0.23f), cos(angle) * 4.6f);
        auto forward = normalize(float3(constant(0.f), -0.2f, 0.f) - eye);
        auto right = normalize(cross(forward, float3(constant(0.f), 1.f, 0.f)));
        auto up = cross(right, forward);
        auto ray = normalize(forward * 1.9f + right * screen.x() + up * screen.y());

        auto travelled = var(0.f);
        auto hit = var(false);
        march(eye, ray, travelled, hit);

        auto colour = var(skyColour(ray));

        auto shade = [&]
        {
            auto position = eye + ray * travelled.get();
            auto lit = shadeSurface(position, ray);
            auto fog = exp(travelled.get() * -0.07f);
            colour = mix(skyColour(ray), lit, fog);
        };

        ifThen(hit.get(), shade);

        setFragment(float4(salted(colour.get()), 1.f));

        recordMilliseconds = millisecondsSince(started);
    }

    void march(const GPU::Float3& eye,
               const GPU::Float3& ray,
               GPU::Var<GPU::Float>& travelled,
               GPU::Var<GPU::Bool>& hit)
    {
        auto step = var(0);

        auto body = [&]
        {
            auto distance = sceneDistance(eye + ray * travelled.get());

            auto arrive = [&]
            {
                hit = true;
                breakLoop();
            };

            ifThen(distance < travelled.get() * 0.0008f + 0.0005f, arrive);
            travelled += distance;
            ifThen(travelled.get() > farPlane, [&] { breakLoop(); });
            step += 1;
        };

        loop(step.get() < marchSteps, body);
    }

    GPU::Float3 shadeSurface(const GPU::Float3& position, const GPU::Float3& ray)
    {
        auto normal = normalAt(position);
        auto toLight = normalize(float3(constant(0.55f), 0.8f, 0.35f));
        auto diffuse = max(dot(normal, toLight), 0.f);
        auto shadow = softShadow(position + normal * 0.004f, toLight);
        auto occlusion = ambientOcclusion(position, normal);
        auto halfway = normalize(toLight - ray);
        auto specular = pow(max(dot(normal, halfway), 0.f), 40.f);
        auto rim = pow(1.f - max(dot(normal, -ray), 0.f), 3.f);

        auto albedo = albedoAt(position);
        auto skyLight = float3(constant(0.35f), 0.45f, 0.65f);

        return albedo * (diffuse * shadow * 1.2f + skyLight * occlusion * 0.6f)
               + specular * shadow * 0.5f + rim * occlusion * 0.15f;
    }

    GPU::Float3 skyColour(const GPU::Float3& ray)
    {
        const auto& first = shapes[0].colour;
        auto horizon = float3(constant(0.75f), 0.78f, 0.82f);
        auto zenith = float3(constant(first[0] * 0.25f),
                             first[1] * 0.25f + 0.1f,
                             first[2] * 0.25f + 0.25f);
        return mix(horizon, zenith, clamp(ray.y() * 1.5f + 0.2f, 0.f, 1.f));
    }

    GPU::Float3 normalAt(const GPU::Float3& position)
    {
        constexpr auto epsilon = 0.0015f;
        auto a = float3(constant(1.f), -1.f, -1.f);
        auto b = float3(constant(-1.f), -1.f, 1.f);
        auto c = float3(constant(-1.f), 1.f, -1.f);
        auto d = float3(constant(1.f), 1.f, 1.f);

        return normalize(a * sceneDistance(position + a * epsilon)
                         + b * sceneDistance(position + b * epsilon)
                         + c * sceneDistance(position + c * epsilon)
                         + d * sceneDistance(position + d * epsilon));
    }

    GPU::Float softShadow(const GPU::Float3& origin, const GPU::Float3& toLight)
    {
        auto light = var(1.f);
        auto along = var(0.02f);
        auto step = var(0);

        auto body = [&]
        {
            auto distance = sceneDistance(origin + toLight * along.get());
            light = min(light.get(), distance * 10.f / along.get());
            along += clamp(distance, 0.03f, 0.3f);

            auto blocked = [&] { breakLoop(); };
            ifThen(light.get() < 0.01f, blocked);
            ifThen(along.get() > 6.f, blocked);
            step += 1;
        };

        loop(step.get() < shadowSteps, body);
        return clamp(light.get(), 0.f, 1.f);
    }

    GPU::Float ambientOcclusion(const GPU::Float3& position,
                                const GPU::Float3& normal)
    {
        auto occluded = constant(0.f);
        auto weight = 1.f;

        for (auto tap = 1; tap <= 4; ++tap)
        {
            const auto reach = 0.03f + 0.09f * (float) tap;
            auto distance = sceneDistance(position + normal * reach);
            occluded = occluded + (reach - distance) * weight;
            weight *= 0.7f;
        }

        return clamp(1.f - occluded * 2.5f, 0.f, 1.f);
    }

    GPU::Float3 albedoAt(const GPU::Float3& position)
    {
        auto weighted = float3(constant(0.f), 0.f, 0.f);
        auto total = constant(0.0001f);
        auto nearest = constant(1000.f);

        for (const auto& shape: shapes)
        {
            auto distance = shapeDistance(position, shape);
            auto weight = exp(abs(distance) * -14.f);
            auto colour =
                float3(constant(shape.colour[0]), shape.colour[1], shape.colour[2]);
            weighted = weighted + colour * weight;
            total = total + weight;
            nearest = min(nearest, distance);
        }

        auto cell = floor(position.x() * 1.2f) + floor(position.z() * 1.2f);
        auto checker = fract(cell * 0.5f) * 2.f;
        auto floorColour = mix(float3(constant(0.32f), 0.33f, 0.36f),
                               float3(constant(0.62f), 0.63f, 0.66f),
                               checker);
        auto onFloor = position.y() - floorHeight < nearest;

        return select(onFloor, floorColour, weighted / total);
    }

    GPU::Float sceneDistance(const GPU::Float3& position)
    {
        constexpr auto blend = 0.22f;
        auto blobs = shapeDistance(position, shapes[0]);

        for (auto index = 1; index < shapes.size(); ++index)
            blobs = smoothMin(blobs, shapeDistance(position, shapes[index]), blend);

        return min(blobs, position.y() - floorHeight);
    }

    static GPU::Float smoothMin(const GPU::Float& a, const GPU::Float& b, float k)
    {
        auto h = max(k - abs(a - b), 0.f) / k;
        return min(a, b) - h * h * (k * 0.25f);
    }

    GPU::Float shapeDistance(const GPU::Float3& position, const Shape& shape)
    {
        auto phase = time * shape.speed + shape.phase;
        auto centre = float3(sin(phase) * shape.orbit + shape.centre[0],
                             cos(phase * 1.3f) * shape.orbit + shape.centre[1],
                             sin(phase * 0.7f) * shape.orbit + shape.centre[2]);
        auto offset = position - centre;

        auto spin = phase * 0.5f;
        auto c = cos(spin);
        auto s = sin(spin);
        auto local = float3(c * offset.x() - s * offset.z(),
                            offset.y(),
                            s * offset.x() + c * offset.z());

        switch (shape.kind)
        {
            case ShapeKind::sphere:
                return length(local) - shape.size;
            case ShapeKind::roundedBox:
            {
                auto q = abs(local) - shape.size * 0.75f;
                return length(max(q, 0.f)) + min(max(q.x(), max(q.y(), q.z())), 0.f)
                       - shape.size * 0.25f;
            }
            case ShapeKind::torus:
            {
                auto ring = float2(length(float2(local.x(), local.z())) - shape.size,
                                   local.y());
                return length(ring) - shape.size * 0.35f;
            }
            case ShapeKind::octahedron:
                return (abs(local.x()) + abs(local.y()) + abs(local.z())
                        - shape.size * 1.3f)
                       * 0.57735f;
        }

        return length(local) - shape.size;
    }

    GPU::Float3 salted(const GPU::Float3& colour)
    {
        if (settings.salt == 0.f)
            return colour;

        return colour + min(0.f, constant(settings.salt));
    }

    GPU::Uniform<GPU::Float4> area;
    GPU::Uniform<GPU::Float> time;
    GPU::Uniform<GPU::Float> aspect;

    int scene;
    Settings settings;
    Vector<Shape> shapes;
    double recordMilliseconds = 0.0;

    EACP_SHADER(area, time, aspect)
};

// What the view prepares every scene with, and so what the warm-up builds the
// pipelines for: a pipeline made for anything else would compile again.
GPU::RenderPipelineDescriptor scenePipeline()
{
    auto descriptor = GPU::RenderPipelineDescriptor {};
    descriptor.sampleCount = 4;
    return descriptor;
}

void addSceneShaders(GPU::ShaderWarmUp& warmUp, const Settings& settings)
{
    for (auto scene = 0; scene < sceneCount; ++scene)
        warmUp.addNamedWithPipeline<SceneShader>(
            nameFor(scene, settings), scenePipeline(), scene, settings);
}

std::string describe(const GPU::ShaderLibraryCache::Stats& before,
                     const GPU::ShaderLibraryCache::Stats& after)
{
    return Strings::concat(after.compiles - before.compiles,
                           " compiled, ",
                           after.hits - before.hits,
                           " found in the cache");
}

// The twelve scenes in a grid, every program prepared against the cache the
// warm-up filled.
struct ScenesView final : GPU::GPUView
{
    explicit ScenesView(const Settings& settings)
    {
        auto& cache = GPU::ShaderLibraryCache::of(GPU::Device::shared());
        const auto before = cache.stats();
        const auto started = Clock::now();
        const auto pipeline = scenePipeline();
        setSampleCount(pipeline.sampleCount);

        for (auto scene = 0; scene < sceneCount; ++scene)
        {
            auto& shader = shaders.createNew(scene, settings);
            shader.setVertices(unitQuad);
            shader.prepare(pipeline);
        }

        LOG("ShaderWarmUp: the view's programs prepared in ",
            (int) millisecondsSince(started),
            " ms, ",
            describe(before, cache.stats()));

        setContinuous(true);
    }

    void update(Threads::FrameTime frameTime) override
    {
        seconds += (float) frameTime.delta;
    }

    void render(GPU::Frame& frame) override
    {
        auto descriptor = GPU::RenderPassDescriptor {};
        descriptor.clearColor = Graphics::Color {0.04f, 0.04f, 0.05f, 1.f};
        descriptor.label = "scenes";
        auto pass = frame.beginPass(descriptor);

        const auto width = 2.f / (float) tileColumns;
        const auto height = 2.f / (float) tileRows;
        const auto size = frame.logicalSize();
        const auto tileAspect = size.y > 0.f ? (size.x * (float) tileRows)
                                                   / (size.y * (float) tileColumns)
                                             : 1.f;

        for (auto index = 0; index < shaders.size(); ++index)
        {
            auto& shader = *shaders[index];
            const auto column = index % tileColumns;
            const auto row = index / tileColumns;

            shader.area = Array {-1.f + width * (float) column + tileGap * 0.5f,
                                 1.f - height * (float) (row + 1) + tileGap * 0.5f,
                                 width - tileGap,
                                 height - tileGap};
            shader.time = seconds;
            shader.aspect = tileAspect;
            pass.draw(shader);
        }
    }

    OwnedVector<SceneShader> shaders;
    float seconds = 3.f;
};

struct ProgressBar final : UI::Component
{
    void setFraction(float fractionToUse)
    {
        fraction = fractionToUse;
        repaint();
    }

    void paint(UI::Graphics& g) override
    {
        auto track = getLocalBounds();
        auto radius = track.h * 0.5f;

        g.setColour(UI::defaultTheme().get(UI::ThemeKeys::panel));
        g.fillRoundedRect(track, radius);

        auto filled = track;
        filled.w = track.w * fraction;

        if (filled.w <= 0.f)
            return;

        g.setColour(UI::defaultTheme().get(UI::ThemeKeys::accent));
        g.fillRoundedRect(filled, radius);
    }

    float fraction = 0.f;
};

std::string headlineFor(const GPU::ShaderWarmUp::Progress& progress)
{
    using State = GPU::ShaderWarmUp::State;

    switch (progress.state)
    {
        case State::idle:
        case State::recording:
            return "Preparing shaders...";
        case State::compiling:
            return Strings::concat(
                "Compiling ", (int) (progress.fraction * 100.f), "%...");
        case State::done:
            return "Compiled";
        case State::cancelled:
            return "Cancelled";
        case State::failed:
            return "Some shaders failed to compile";
    }

    return {};
}

std::string detailFor(const GPU::ShaderWarmUp::Progress& progress)
{
    if (progress.state == GPU::ShaderWarmUp::State::recording)
        return "Recording and emitting every shader";

    if (progress.current.empty())
        return Strings::concat(progress.completed, " of ", progress.total);

    return Strings::concat(progress.current,
                           "  (",
                           progress.completed + 1,
                           " of ",
                           progress.total,
                           ")");
}

struct ProgressPanel final : UI::Component
{
    ProgressPanel()
    {
        headline.setFontSize(22.f);
        headline.setJustification(UI::Justification::Centred);
        detail.setThemeColour(UI::ThemeKeys::dimText);
        detail.setJustification(UI::Justification::Centred);
        addChildren({headline, bar, detail});
    }

    void show(const GPU::ShaderWarmUp::Progress& progress)
    {
        headline.setText(headlineFor(progress));
        bar.setFraction(progress.fraction);
        detail.setText(detailFor(progress));
    }

    void paint(UI::Graphics& g) override
    {
        g.fillAll(UI::defaultTheme().get(UI::ThemeKeys::background));
    }

    void resized() override
    {
        constexpr auto columnWidth = 420.f;
        auto area = getLocalBounds();
        auto column = UI::Rect {
            (area.w - columnWidth) * 0.5f, area.h * 0.5f - 50.f, columnWidth, 100.f};

        headline.setBounds(column.removeFromTop(36.f));
        column.removeFromTop(12.f);
        bar.setBounds(column.removeFromTop(12.f));
        column.removeFromTop(12.f);
        detail.setBounds(column.removeFromTop(24.f));
    }

    UI::Label headline;
    ProgressBar bar;
    UI::Label detail;
};

struct Durations
{
    void add(double milliseconds) { samples.add(milliseconds); }

    std::string describe() const
    {
        if (samples.size() == 0)
            return "none";

        auto sorted = samples;
        sorted.sort();

        auto at = [&sorted](double fraction)
        { return sorted[(int) (fraction * (double) (sorted.size() - 1))]; };

        return Strings::format("{} samples, median {:.1f} ms, p95 {:.1f} ms, "
                               "max {:.1f} ms",
                               sorted.size(),
                               at(0.5),
                               at(0.95),
                               sorted[sorted.size() - 1]);
    }

    Vector<double> samples;
};

struct PhaseDurations
{
    Durations pingLateness;
    Durations setSize;
    Durations render;
};

struct ProgressHost final : UI::ComponentHost
{
    explicit ProgressHost(const GPU::ShaderWarmUp& warmUpToUse)
        : warmUp(warmUpToUse)
    {
        setFontPointSize(14.f);
        setRootComponent(panel);
        refresh();
    }

    void refresh() { panel.show(warmUp.progress()); }

    void render(GPU::Frame& frame) override
    {
        const auto started = Clock::now();
        UI::ComponentHost::render(frame);
        onRendered(millisecondsSince(started));
    }

    const GPU::ShaderWarmUp& warmUp;
    ProgressPanel panel;
    Threads::Timer poll {[this] { refresh(); }, 30};
    std::function<void(double)> onRendered = [](double) {};
};

// --measure-responsiveness: a 250 Hz timer measures how late the main thread
// runs it and a 20 Hz one resizes the window, timing each setSize and each
// frame the progress screen renders - while the warm-up compiles, then for a
// few seconds after it, through the swap to the scenes, whose preparing is
// where a pipeline left to the main thread shows up as one long stall.
struct ResponsivenessProbe
{
    static constexpr auto pingHz = 250;
    static constexpr auto resizeHz = 20;

    ResponsivenessProbe(Graphics::Window& windowToUse,
                        const GPU::ShaderWarmUp& warmUpToUse,
                        ProgressHost& host)
        : window(windowToUse)
        , warmUp(warmUpToUse)
    {
        host.onRendered = [this](double milliseconds)
        { phase().render.add(milliseconds); };
    }

    PhaseDurations& phase() { return warmUp.isRunning() ? compiling : afterwards; }

    void ping()
    {
        const auto now = Clock::now();
        const auto interval =
            std::chrono::duration<double, std::milli>(now - lastPing).count();
        lastPing = now;
        phase().pingLateness.add(std::max(0.0, interval - 1000.0 / pingHz));
    }

    void resize()
    {
        const auto wide = (resizes++ % 2) == 0;
        const auto size =
            wide ? Graphics::Point {1100.f, 640.f} : Graphics::Point {900.f, 520.f};
        const auto started = Clock::now();
        window.setSize(size);
        phase().setSize.add(millisecondsSince(started));
    }

    static void log(const std::string& name, const PhaseDurations& durations)
    {
        LOG("ShaderWarmUp: ",
            name,
            " ping lateness: ",
            durations.pingLateness.describe());
        LOG("ShaderWarmUp: ", name, " setSize: ", durations.setSize.describe());
        LOG("ShaderWarmUp: ",
            name,
            " progress screen render: ",
            durations.render.describe());
    }

    void report() const
    {
        log("while compiling,", compiling);
        log("afterwards,", afterwards);
    }

    Graphics::Window& window;
    const GPU::ShaderWarmUp& warmUp;
    PhaseDurations compiling;
    PhaseDurations afterwards;
    Clock::time_point lastPing = Clock::now();
    int resizes = 0;
    Threads::Timer pinger {[this] { ping(); }, pingHz};
    Threads::Timer resizer {[this] { resize(); }, resizeHz};
};

void logResult(const GPU::ShaderWarmUp::Result& result)
{
    for (const auto& timing: result.timings)
        LOG("ShaderWarmUp: ",
            timing.name,
            " compiled in ",
            timing.compile.count,
            " ms");

    LOG("ShaderWarmUp: ",
        result.total,
        " shaders in ",
        result.elapsed.count,
        " ms (",
        result.recording.count,
        " ms recording and emitting, ",
        result.elapsed.count - result.recording.count,
        " ms compiling) - ",
        result.compiled,
        " compiled, ",
        result.alreadyCached,
        " already cached, ",
        result.precompiled,
        " precompiled, ",
        result.pipelines,
        " pipelines built, ",
        result.failures.size(),
        " failed",
        result.cancelled ? " (cancelled)" : "");

    for (const auto& failure: result.failures)
        LOG("ShaderWarmUp: ", failure.name, "\n", failure.compilation.describe());
}

GPU::ShaderWarmUp::Options makeWarmUpOptions(int delayMs)
{
    auto options = GPU::ShaderWarmUp::Options {};
    options.simulatedCompileTime = Time::MS {delayMs};
    return options;
}

Graphics::WindowOptions makeWindowOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = 960;
    options.height = 540;
    options.title = "eacp GPU - shaders warmed up on a worker";
    options.minWidth = 480;
    options.minHeight = 270;
    return options;
}

struct DemoApp
{
    DemoApp(const Settings& settingsToUse, int delayMs, bool measure)
        : settings(settingsToUse)
        , warmUp(makeWarmUpOptions(delayMs))
    {
        content.set(makeOwned<ProgressHost>(warmUp));

        if (measure)
            probe = makeOwned<ResponsivenessProbe>(
                window, warmUp, static_cast<ProgressHost&>(*content));

        addSceneShaders(warmUp, settings);
        UI::addComponentHostShaders(warmUp);

        auto reportAndQuit = [this]
        {
            probe->report();
            Apps::quit();
        };

        auto finished =
            [this, reportAndQuit](const GPU::ShaderWarmUp::Result& result)
        {
            logResult(result);
            content.set(makeOwned<ScenesView>(settings));

            if (probe != nullptr)
                Threads::callAfter(Time::MS {4500}, reportAndQuit);
        };

        warmUp.start().then(finished);
    }

    Settings settings;
    GPU::ShaderWarmUp warmUp;
    OwningPointer<ResponsivenessProbe> probe;
    Graphics::OptionalView<> content;
    Graphics::Window window {content, makeWindowOptions()};
};

bool saveFrame(GPU::GPUView& view, const FilePath& path)
{
    view.setBounds({0.f, 0.f, 960.f, 540.f});
    view.resized();
    view.renderToImage(1.f);
    auto image = view.renderToImage(1.f);

    if (!image.isValid())
        return false;

    image.save(path);
    return true;
}

void logFrameCost(GPU::GPUView& view, float scale)
{
    constexpr auto frames = 10;
    view.renderToImage(scale);
    const auto started = Clock::now();

    for (auto frame = 0; frame < frames; ++frame)
        view.renderToImage(scale);

    LOG("ShaderWarmUp: a ",
        (int) (960.f * scale),
        "x",
        (int) (540.f * scale),
        " frame of the grid takes ",
        millisecondsSince(started) / frames,
        " ms, read back included; the GPU took ",
        GPU::Device::shared().lastFrameTimings().milliseconds,
        " ms for the last one timed");
}

// While it compiles, the progress screen goes to <stem>-progress.png beside
// `path`, taken once the warm-up is a third of the way through.
int snapshot(const FilePath& path, const Settings& settings, int delayMs)
{
    auto exitCode = 1;

    auto render = [&]
    {
        auto warmUp = GPU::ShaderWarmUp {makeWarmUpOptions(delayMs)};
        addSceneShaders(warmUp, settings);
        auto done = warmUp.start();

        auto aThirdThrough = [&warmUp]
        {
            const auto progress = warmUp.progress();
            return progress.isFinished() || progress.completed * 3 >= sceneCount;
        };

        Threads::runEventLoopUntil(aThirdThrough, Time::MS {600000}, Time::MS {1});

        auto progressScreen = ProgressHost {warmUp};
        const auto progressPath = path.sibling(path.stem() + "-progress.png");
        const auto progressSaved = saveFrame(progressScreen, progressPath);

        logResult(done.waitFor(Time::MS {600000}));

        auto view = ScenesView {settings};

        if (progressSaved && saveFrame(view, path))
            exitCode = 0;

        logFrameCost(view, 1.f);
        logFrameCost(view, 2.f);
    };

    Apps::run(render);
    return exitCode;
}

float launchSalt()
{
    const auto now = Clock::now().time_since_epoch().count();
    return (float) (1 + (std::uint64_t) now % 999983u);
}

bool isSet(const std::string& variable)
{
    return Strings::parseIntOr(getEnvValue(variable), 0) != 0;
}
} // namespace ShaderWarmUpDemo

int main(int argc, char** argv)
{
    using namespace ShaderWarmUpDemo;

    auto snapshotPath = FilePath {};
    auto delayMs =
        Strings::parseIntOr(getEnvValue("EACP_SHADER_WARMUP_DELAY_MS"), 0);
    auto complexity = Strings::parseIntOr(
        getEnvValue("EACP_SHADER_WARMUP_COMPLEXITY"), defaultComplexity());
    auto reuseSystemCache = isSet("EACP_SHADER_WARMUP_REUSE_SYSTEM_CACHE");
    auto measureResponsiveness = false;

    auto cli = Args::Parser {"ShaderWarmUp",
                             "Shaders compiled on a worker before the view exists."};
    cli.option("--complexity",
               complexity,
               "shapes per scene, which the compile grows with")
        .atLeast(1);
    cli.flag("--reuse-system-cache",
             reuseSystemCache,
             "leave out the per-launch salt, so a second launch can hit the "
             "system's compile cache");
    cli.option("--delay-ms", delayMs, "hold each shader in flight this much longer")
        .atLeast(0);
    cli.flag("--measure-responsiveness",
             measureResponsiveness,
             "resize the window while it compiles, log how late the main "
             "thread ran and how long each resize and frame took, and exit");
    cli.positional(snapshotPath,
                   "snapshot",
                   "warm up, render one frame to this PNG and exit, with no window");

    if (auto code = cli.exitCodeFor(cli.parse(argc, argv)))
        return *code;

    auto settings = Settings {};
    settings.complexity = complexity;
    settings.salt = reuseSystemCache ? 0.f : launchSalt();

    LOG("ShaderWarmUp: ",
        sceneCount,
        " scenes of ",
        settings.complexity,
        " shapes, ",
        reuseSystemCache ? "no salt"
                         : Strings::concat("salt ", (int) settings.salt));

    if (!snapshotPath.empty())
        return snapshot(snapshotPath, settings, delayMs);

    return Apps::run<DemoApp>(settings, delayMs, measureResponsiveness);
}
