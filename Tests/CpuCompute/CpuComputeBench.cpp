#include <eacp/GPU/CpuCompute/CpuCompute.h>

#if EACP_BENCH_PATHS
#include <eacp/GPUWidgets/GPUWidgets.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

// The CPU executor against the loop a person would have written instead: the
// same arithmetic in the same order, compiled with the same flags (-O3, no
// multiply-add contraction, no errno), so the two agree bit for bit and the
// ratio is the interpreter's overhead and nothing else.
//
// The stream kernels are the example apps' own (Apps/GPU/Compute and
// Apps/GPU/AsyncCompute) over a million elements; BinKernel runs over the paths
// Apps/GPU/PathBench times, one path at a time and all of them as one batch.

using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::GPU::CpuCompute;

namespace
{
constexpr auto twoPi = 2.0f * std::numbers::pi_v<float>;
constexpr auto streamCount = 1 << 20;
constexpr auto budgetMs = 300.0;

using Clock = std::chrono::steady_clock;
using Floats = Vector<float>;
using UInts = Vector<std::uint32_t>;

volatile std::uint64_t gSink = 0;
auto gMismatches = 0;

struct Timing
{
    double best = 0.0;
    double median = 0.0;
    int runs = 0;
};

double millisecondsOf(Clock::duration elapsed)
{
    return std::chrono::duration<double, std::milli>(elapsed).count();
}

// Every run timed on its own, with whatever the run needs reset beforehand kept
// out of the clock: a binning pass accumulates into its arrays, so each one has
// to start from zeroes it did not pay for. As many runs as fit the budget,
// never fewer than five, and the median is what is reported.
template <typename Prepare, typename Run>
Timing timeRuns(Prepare&& prepare, Run&& run)
{
    prepare();
    run();

    prepare();
    auto start = Clock::now();
    run();
    auto estimate = std::max(millisecondsOf(Clock::now() - start), 1e-4);
    auto runs = std::clamp((int) (budgetMs / estimate), 5, 2000);

    auto samples = Vector<double> {};

    for (auto i = 0; i < runs; ++i)
    {
        prepare();
        auto begin = Clock::now();
        run();
        samples.add(millisecondsOf(Clock::now() - begin));
    }

    std::sort(samples.begin(), samples.end());
    return {samples.front(), samples[samples.size() / 2], runs};
}

void noPreparation() {}

void printHeader(const char* what, const char* unit)
{
    std::printf("  %-34s %9s | %11s %11s %8s | %9s %9s | %9s\n",
                what,
                unit,
                "interp ms",
                "hand ms",
                "ratio",
                "interp ns",
                "hand ns",
                "footprint");
}

void printRow(const char* name,
              long long items,
              const Timing& interpreted,
              const Timing& handWritten,
              std::size_t footprint,
              bool same)
{
    auto perItem = [&](double ms) { return ms * 1e6 / (double) items; };

    std::printf("  %-34s %9lld | %11.3f %11.3f %7.2fx | %9.2f %9.2f | %8.1fK%s\n",
                name,
                items,
                interpreted.median,
                handWritten.median,
                interpreted.median / handWritten.median,
                perItem(interpreted.median),
                perItem(handWritten.median),
                (double) footprint / 1024.0,
                same ? "" : "   <-- MISMATCH");

    if (!same)
        ++gMismatches;
}

bool reportIfDifferent(const char* name, int index, std::uint32_t a, std::uint32_t b)
{
    std::printf("  !! %s: first difference at %d (interpreter 0x%08x, hand-written "
                "0x%08x)\n",
                name,
                index,
                a,
                b);
    return false;
}

bool sameBits(const char* name, const Floats& interpreted, const Floats& handWritten)
{
    for (auto i = 0; i < interpreted.size(); ++i)
        if (std::bit_cast<std::uint32_t>(interpreted[i])
            != std::bit_cast<std::uint32_t>(handWritten[i]))
            return reportIfDifferent(name,
                                     i,
                                     std::bit_cast<std::uint32_t>(interpreted[i]),
                                     std::bit_cast<std::uint32_t>(handWritten[i]));

    return true;
}

bool sameWords(const char* name, const UInts& interpreted, const UInts& handWritten)
{
    for (auto i = 0; i < interpreted.size(); ++i)
        if (interpreted[i] != handWritten[i])
            return reportIfDifferent(name, i, interpreted[i], handWritten[i]);

    return true;
}

Floats floatsOf(int count)
{
    auto values = Floats {};
    values.resize(count, 0.f);
    return values;
}

UInts uintsOf(int count)
{
    auto values = UInts {};
    values.resize(count, 0u);
    return values;
}

// ------------------------------------------------------------- stream kernels

struct ToneKernel final : ComputeKernel
{
    ToneKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto phase = toFloat(i) / sampleRate * frequency * twoPi;
        auto wave = sin(phase) + sin(phase * 2.0f) / 2.0f + sin(phase * 3.0f) / 3.0f;
        write(output, i, wave);
    }

    Uniform<OutputBuffer> output;
    Uniform<Float> frequency;
    Uniform<Float> sampleRate;
    EACP_SHADER(output, frequency, sampleRate)
};

struct CrossfadeKernel final : ComputeKernel
{
    CrossfadeKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto mixed = mix(toneA[i], toneB[i], blend);
        auto shaped = mixed / (abs(mixed) + 1.0f);
        write(output, i, shaped * gain);
    }

    Uniform<InputBuffer> toneA;
    Uniform<InputBuffer> toneB;
    Uniform<OutputBuffer> output;
    Uniform<Float> blend;
    Uniform<Float> gain;
    EACP_SHADER(toneA, toneB, output, blend, gain)
};

struct SmoothKernel final : ComputeKernel
{
    SmoothKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto previous = input[(i + length - 1u) % length];
        auto next = input[(i + 1u) % length];
        write(output, i, (previous + input[i] + next) / 3.0f);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<UInt> length;
    EACP_SHADER(input, output, length)
};

struct MixKernel final : ComputeKernel
{
    MixKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto x = toFloat(i) * scale;
        auto wave = sin(x) * cos(x * 1.7f) + sin(x * 0.3f) * 0.5f;
        auto shaped = wave / (abs(wave) + 1.0f);
        write(output, i, shaped * gain);
    }

    Uniform<OutputBuffer> output;
    Uniform<Float> scale;
    Uniform<Float> gain;
    EACP_SHADER(output, scale, gain)
};

void handTone(float* output, int count, float frequency, float sampleRate)
{
    for (auto i = 0; i < count; ++i)
    {
        auto phase = (float) (std::uint32_t) i / sampleRate * frequency * twoPi;
        output[i] = std::sin(phase) + std::sin(phase * 2.0f) / 2.0f
                    + std::sin(phase * 3.0f) / 3.0f;
    }
}

void handCrossfade(const float* toneA,
                   const float* toneB,
                   float* output,
                   int count,
                   float blend,
                   float gain)
{
    for (auto i = 0; i < count; ++i)
    {
        auto mixed = toneA[i] + (toneB[i] - toneA[i]) * blend;
        auto shaped = mixed / (std::abs(mixed) + 1.0f);
        output[i] = shaped * gain;
    }
}

void handSmooth(const float* input, float* output, int count, std::uint32_t length)
{
    for (auto i = 0u; i < (std::uint32_t) count; ++i)
    {
        auto previous = input[(i + length - 1u) % length];
        auto next = input[(i + 1u) % length];
        output[i] = (previous + input[i] + next) / 3.0f;
    }
}

void handMix(float* output, int count, float scale, float gain)
{
    for (auto i = 0; i < count; ++i)
    {
        auto x = (float) (std::uint32_t) i * scale;
        auto wave = std::sin(x) * std::cos(x * 1.7f) + std::sin(x * 0.3f) * 0.5f;
        auto shaped = wave / (std::abs(wave) + 1.0f);
        output[i] = shaped * gain;
    }
}

// The caller-supplied threads Executor::dispatchGroups is for, as an audio
// host's worker pool would be: made once, outside every timed run, each with a
// workspace of its own, and woken per run to take an equal share of the groups.
// The calling thread takes the first share.
class Crew
{
public:
    Crew(const Executor& executorToRun, int threads)
        : executor(executorToRun)
    {
        workspaces.reserve((std::size_t) threads);

        for (auto k = 0; k < threads; ++k)
            workspaces.emplace_back(executor.plan());

        for (auto k = 1; k < threads; ++k)
            workers.emplace_back([this, k] { work(k); });
    }

    ~Crew()
    {
        stopping = true;
        generation.fetch_add(1, std::memory_order_release);
        generation.notify_all();

        for (auto& worker: workers)
            worker.join();
    }

    void run(const PreparedDispatch& prepared)
    {
        current = &prepared;
        remaining.store((int) workers.size(), std::memory_order_relaxed);
        generation.fetch_add(1, std::memory_order_release);
        generation.notify_all();

        runShare(0);

        for (auto left = remaining.load(std::memory_order_acquire); left != 0;
             left = remaining.load(std::memory_order_acquire))
            remaining.wait(left, std::memory_order_acquire);
    }

    std::size_t footprintBytes() const
    {
        return executor.plan().footprintBytes() * workspaces.size();
    }

private:
    void work(int share)
    {
        auto seen = std::uint64_t {0};

        while (true)
        {
            generation.wait(seen, std::memory_order_acquire);
            seen = generation.load(std::memory_order_acquire);

            if (stopping)
                return;

            runShare(share);

            if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
                remaining.notify_one();
        }
    }

    void runShare(int share)
    {
        auto threads = (std::int64_t) workspaces.size();
        auto each = (current->groupCount() + threads - 1) / threads;
        executor.dispatchGroups(
            *current, share * each, each, workspaces[(std::size_t) share]);
    }

    const Executor& executor;
    std::vector<Workspace> workspaces;
    std::vector<std::thread> workers;
    const PreparedDispatch* current = nullptr;
    std::atomic<std::uint64_t> generation {0};
    std::atomic<int> remaining {0};
    std::atomic<bool> stopping {false};
};

int crewSize()
{
    return std::max(1, (int) std::thread::hardware_concurrency());
}

// The same kernel with its groups shared over every hardware thread, against
// the same single-threaded hand-written loop.
template <typename Bind>
void benchStreamOnThreads(const std::string& name,
                          Executor& executor,
                          Bind&& bind,
                          const Timing& handTime,
                          const Floats& handWritten)
{
    auto threads = crewSize();
    auto interpreted = floatsOf(streamCount);
    auto bindings = Bindings {};
    bind(bindings, interpreted);

    auto crew = Crew {executor, threads};
    auto crewTime =
        timeRuns(noPreparation,
                 [&] { crew.run(executor.prepareDispatch(bindings, streamCount)); });

    gSink += std::bit_cast<std::uint32_t>(interpreted[streamCount / 3]);

    auto label = name + " x" + std::to_string(threads);
    printRow(label.c_str(),
             streamCount,
             crewTime,
             handTime,
             crew.footprintBytes(),
             sameBits(label.c_str(), interpreted, handWritten));
}

// One stream kernel against its hand-written twin: both write a fresh output
// of the same length, timed, and compared bit for bit.
template <typename Kernel, typename Bind, typename Hand>
void benchStream(
    const char* name, PlanOptions options, Kernel& kernel, Bind&& bind, Hand&& hand)
{
    auto executor = Executor {kernel, options};

    if (!executor.isValid())
    {
        std::printf("  %-34s invalid plan: %s\n", name, executor.reason().c_str());
        ++gMismatches;
        return;
    }

    auto interpreted = floatsOf(streamCount);
    auto handWritten = floatsOf(streamCount);

    auto bindings = Bindings {};
    bind(bindings, interpreted);

    auto interpretedTime =
        timeRuns(noPreparation, [&] { executor.dispatch(bindings, streamCount); });
    auto handTime = timeRuns(noPreparation, [&] { hand(handWritten); });

    gSink += std::bit_cast<std::uint32_t>(interpreted[streamCount / 3]);
    gSink += std::bit_cast<std::uint32_t>(handWritten[streamCount / 3]);

    printRow(name,
             streamCount,
             interpretedTime,
             handTime,
             executor.plan().footprintBytes(),
             sameBits(name, interpreted, handWritten));

    if (crewSize() > 1)
        benchStreamOnThreads(name, executor, bind, handTime, handWritten);
}

void benchStreams(PlanOptions options)
{
    std::printf("stream kernels, %d elements, batches of up to %d lanes:\n",
                streamCount,
                options.targetBatchLanes);

    if (crewSize() > 1)
        std::printf("(a row ending xN splits the groups over N threads through "
                    "dispatchGroups, the threads made outside the timed runs; its "
                    "ratio is against the single-threaded loop)\n");

    printHeader("kernel", "elements");

    constexpr auto frequencyA = 440.f;
    constexpr auto frequencyB = 660.f;
    constexpr auto sampleRate = 48000.f;

    auto toneA = floatsOf(streamCount);
    auto toneB = floatsOf(streamCount);
    handTone(toneA.data(), streamCount, frequencyA, sampleRate);
    handTone(toneB.data(), streamCount, frequencyB, sampleRate);

    auto tone = ToneKernel {};
    tone.frequency = frequencyA;
    tone.sampleRate = sampleRate;
    benchStream(
        "Compute/ToneKernel",
        options,
        tone,
        [&](Bindings& bindings, Floats& output)
        { bindings.set(tone.output, output); },
        [&](Floats& output)
        { handTone(output.data(), streamCount, frequencyA, sampleRate); });

    auto crossfade = CrossfadeKernel {};
    crossfade.blend = 0.3f;
    crossfade.gain = 0.8f;
    benchStream(
        "Compute/CrossfadeKernel",
        options,
        crossfade,
        [&](Bindings& bindings, Floats& output)
        {
            bindings.set(crossfade.toneA, toneA);
            bindings.set(crossfade.toneB, toneB);
            bindings.set(crossfade.output, output);
        },
        [&](Floats& output)
        {
            handCrossfade(
                toneA.data(), toneB.data(), output.data(), streamCount, 0.3f, 0.8f);
        });

    auto blended = floatsOf(streamCount);
    handCrossfade(
        toneA.data(), toneB.data(), blended.data(), streamCount, 0.3f, 0.8f);

    auto smooth = SmoothKernel {};
    smooth.length = (std::uint32_t) streamCount;
    benchStream(
        "Compute/SmoothKernel",
        options,
        smooth,
        [&](Bindings& bindings, Floats& output)
        {
            bindings.set(smooth.input, blended);
            bindings.set(smooth.output, output);
        },
        [&](Floats& output)
        {
            handSmooth(blended.data(),
                       output.data(),
                       streamCount,
                       (std::uint32_t) streamCount);
        });

    auto mixKernel = MixKernel {};
    mixKernel.scale = 0.001f;
    mixKernel.gain = 0.9f;
    benchStream(
        "AsyncCompute/MixKernel",
        options,
        mixKernel,
        [&](Bindings& bindings, Floats& output)
        { bindings.set(mixKernel.output, output); },
        [&](Floats& output) { handMix(output.data(), streamCount, 0.001f, 0.9f); });
}

#if EACP_BENCH_PATHS
// ------------------------------------------------------------------ BinKernel

using Graphics::Point;
using Graphics::Rect;

// Apps/GPU/PathBench's paths, unchanged.
Point onCircle(Point centre, float radius, float angle)
{
    return {centre.x + std::sin(angle) * radius,
            centre.y - std::cos(angle) * radius};
}

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

    path.lineTo({width, height});
    path.lineTo({0.f, height});
    path.close();

    return path;
}

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

struct Case
{
    const char* name;
    GPUWidgets::Path path;
};

Vector<Case> pathBenchCases()
{
    auto cases = Vector<Case> {};
    cases.add({"knob indicator, 40pt", knobIndicator(40.f, 0.72f)});
    cases.add({"knob indicator, 96pt", knobIndicator(96.f, 0.72f)});
    cases.add({"PathQuality panel", qualityPanel({0.f, 0.f, 304.f, 497.f})});
    cases.add({"automation curve, 1200pt", automationCurve(1200.f, 192.f, 40)});
    cases.add({"full-window ellipse", fullWindowEllipse(1600.f, 1000.f)});
    cases.add({"artwork, 4k segments", denseArtwork(900.f, 40, 100)});
    cases.add({"artwork, 20k segments", denseArtwork(900.f, 100, 200)});
    cases.add({"artwork, 100k segments", denseArtwork(900.f, 200, 500)});
    return cases;
}

constexpr auto pathScale = 2.f;
constexpr auto tileSize = GPUWidgets::PathIndexedKernel::tileSize;
constexpr auto tileEdge = (float) tileSize;
constexpr auto countMode = GPUWidgets::BinKernel::countMode;
constexpr auto fillMode = GPUWidgets::BinKernel::fillMode;

// CoverageBatch::add, gathered by hand as PathKernelCpuTests does, so both
// sides get the batch's arrays rather than its private buffers.
struct Scene
{
    Floats segments;
    Floats records;
    Floats segmentStarts;

    int paths = 0;
    int cells = 0;
    int tiles = 0;
    int entries = 0;

    int segmentCount() const { return segments.size() / 4; }
    int tileSlots() const { return tiles + 1; }
};

void gatherInto(Scene& scene, const GPUWidgets::Path& path)
{
    auto rasterizer = GPUWidgets::PathRasterizer {};
    rasterizer.setScale(pathScale);
    rasterizer.setPath(path);

    if (rasterizer.isEmpty())
        return;

    scene.segmentStarts.add((float) scene.segmentCount());

    scene.records.add((float) scene.cells);
    scene.records.add((float) rasterizer.getCoverageWidth());
    scene.records.add((float) rasterizer.getCoverageHeight());
    scene.records.add((float) scene.tiles);

    for (auto i = 0; i < 4; ++i)
        scene.records.add(0.f);

    for (auto value: rasterizer.getSegments())
        scene.segments.add(value);

    scene.cells += rasterizer.getCellCount();
    scene.tiles += rasterizer.getTileCount();
    scene.entries += rasterizer.getEntryBound();
    ++scene.paths;
}

void finish(Scene& scene)
{
    scene.segmentStarts.add((float) scene.segmentCount());
}

// The arrays one side bins into. The offsets are the prefix sum of what the
// count left, and the counts are zeroed behind it, exactly as PrefixSum leaves
// them for the fill.
struct BinArrays
{
    explicit BinArrays(const Scene& scene)
        : cells(uintsOf(scene.cells))
        , counts(uintsOf(scene.tileSlots()))
        , offsets(uintsOf(scene.tileSlots()))
        , entries(floatsOf(4 * std::max(1, scene.entries)))
    {
    }

    void clearForCount()
    {
        std::fill(cells.begin(), cells.end(), 0u);
        std::fill(counts.begin(), counts.end(), 0u);
    }

    void sumForFill()
    {
        auto running = std::uint32_t {0};

        for (auto i = 0; i < counts.size(); ++i)
        {
            offsets[i] = running;
            running += counts[i];
        }
    }

    void clearForFill() { std::fill(counts.begin(), counts.end(), 0u); }

    UInts cells;
    UInts counts;
    UInts offsets;
    Floats entries;
};

int tileOf(float coordinate)
{
    return (int) std::floor(coordinate * (1.f / tileEdge));
}

int tileAfter(float coordinate)
{
    return (int) std::ceil(coordinate * (1.f / tileEdge));
}

std::uint32_t tilesWideOf(std::uint32_t width)
{
    return (width + (unsigned) (tileSize - 1)) / (unsigned) tileSize;
}

void handCrossing(BinArrays& arrays,
                  std::uint32_t cellBase,
                  std::uint32_t height,
                  std::uint32_t tilesWide,
                  std::uint32_t column,
                  float top,
                  float bottom,
                  float winding)
{
    if (column >= tilesWide)
        return;

    auto columnBase = cellBase + column * height;
    auto last = std::min((int) std::ceil(bottom), (int) height);

    for (auto row = std::max((int) std::floor(top), 0); row < last; ++row)
    {
        auto rowTop = std::max(top, (float) row);
        auto rowBottom = std::min(bottom, (float) (row + 1));
        auto covered = rowBottom - rowTop;

        if (covered > 0.f)
        {
            auto scaled = covered * winding + 0.5f;
            arrays.cells[(int) (columnBase + (std::uint32_t) row)] +=
                (std::uint32_t) (int) std::floor(scaled);
        }
    }
}

// BinKernel, written as the loop over paths and their segments a person would
// write on the CPU: the same clip, the same crossings, the same filing.
void handBin(const Scene& scene, BinArrays& arrays, unsigned mode)
{
    auto capacity = (std::uint32_t) scene.entries;

    for (auto path = 0; path < scene.paths; ++path)
    {
        const auto* record = scene.records.data() + path * 8;
        auto cellBase = (std::uint32_t) record[0];
        auto height = (std::uint32_t) record[2];
        auto tilesWide = tilesWideOf((std::uint32_t) record[1]);
        auto tilesHigh = tilesWideOf(height);
        auto tileBase = (std::uint32_t) record[3];

        auto first = (int) scene.segmentStarts[path];
        auto end = (int) scene.segmentStarts[path + 1];

        for (auto item = first; item < end; ++item)
        {
            const auto* segment = scene.segments.data() + item * 4;
            auto fromX = segment[0];
            auto fromY = segment[1];
            auto topY = std::min(segment[1], segment[3]);
            auto bottomY = std::max(segment[1], segment[3]);
            auto slope = (segment[2] - segment[0]) / (segment[3] - segment[1]);
            auto winding = segment[3] > segment[1] ? GPUWidgets::backdropFixedScale
                                                   : -GPUWidgets::backdropFixedScale;

            auto lastRow = std::min(tileAfter(bottomY) - 1, (int) tilesHigh - 1);

            for (auto row = std::max(tileOf(topY), 0); row <= lastRow; ++row)
            {
                auto bandTop = std::max(topY, (float) row * tileEdge);
                auto bandBottom = std::min(bottomY, (float) (row + 1) * tileEdge);

                if (!(bandBottom > bandTop))
                    continue;

                auto enters = fromX + (bandTop - fromY) * slope;
                auto leaves = fromX + (bandBottom - fromY) * slope;
                auto beyond = std::max(tileAfter(std::max(enters, leaves)), 0);

                if (mode == countMode)
                    handCrossing(arrays,
                                 cellBase,
                                 height,
                                 tilesWide,
                                 (std::uint32_t) beyond,
                                 bandTop,
                                 bandBottom,
                                 winding);

                auto lastColumn = std::min(beyond - 1, (int) tilesWide - 1);

                for (auto column = std::max(tileOf(std::min(enters, leaves)), 0);
                     column <= lastColumn;
                     ++column)
                {
                    auto tile = tileBase + (std::uint32_t) row * tilesWide
                                + (std::uint32_t) column;

                    if (mode == countMode)
                    {
                        ++arrays.counts[(int) tile];
                        continue;
                    }

                    auto at =
                        arrays.offsets[(int) tile] + arrays.counts[(int) tile]++;

                    if (at < capacity)
                        std::memcpy(arrays.entries.data() + 4 * at,
                                    segment,
                                    4 * sizeof(float));
                }
            }
        }
    }
}

// Where the fill puts a segment inside its own tile's run is whatever the
// tile's cursor handed out, and the interpreter hands them out a group of lanes
// at a time rather than a segment at a time. So each tile's run is compared as
// a set, each entry in it bit for bit.
bool sameRuns(const char* name, const BinArrays& interpreted, const BinArrays& hand)
{
    using Entry = std::array<std::uint32_t, 4>;

    auto runOf = [](const BinArrays& arrays, int tile)
    {
        auto run = Vector<Entry> {};
        auto begin = (int) arrays.offsets[tile];

        for (auto i = 0; i < (int) arrays.counts[tile]; ++i)
        {
            auto entry = Entry {};
            std::memcpy(entry.data(),
                        arrays.entries.data() + 4 * (begin + i),
                        sizeof(Entry));
            run.add(entry);
        }

        std::sort(run.begin(), run.end());
        return run;
    };

    for (auto tile = 0; tile + 1 < interpreted.counts.size(); ++tile)
    {
        if (!(runOf(interpreted, tile) == runOf(hand, tile)))
        {
            std::printf("  !! %s: tile %d holds different segments\n", name, tile);
            return false;
        }
    }

    return true;
}

void benchScene(const char* name, const Scene& scene)
{
    auto kernel = GPUWidgets::BinKernel {};
    kernel.entryCapacity = (std::uint32_t) scene.entries;
    kernel.pathCount = scene.paths;

    auto executor = Executor {kernel};

    if (!executor.isValid())
    {
        std::printf("  %-34s invalid plan: %s\n", name, executor.reason().c_str());
        ++gMismatches;
        return;
    }

    auto interpreted = BinArrays {scene};
    auto hand = BinArrays {scene};

    auto bindings = Bindings {};
    bindings.set(kernel.segments, scene.segments);
    bindings.set(kernel.records, scene.records);
    bindings.set(kernel.pathStarts, scene.segmentStarts);
    bindings.set(kernel.cells, interpreted.cells);
    bindings.set(kernel.tileCounts, interpreted.counts);
    bindings.set(kernel.tileOffsets, interpreted.offsets);
    bindings.set(kernel.tileSegments, interpreted.entries);

    auto segments = scene.segmentCount();
    auto footprint = executor.plan().footprintBytes();

    kernel.mode = countMode;

    auto countInterpreted = timeRuns([&] { interpreted.clearForCount(); },
                                     [&] { executor.dispatch(bindings, segments); });
    auto countHand = timeRuns([&] { hand.clearForCount(); },
                              [&] { handBin(scene, hand, countMode); });

    auto counted = sameWords(name, interpreted.cells, hand.cells)
                   && sameWords(name, interpreted.counts, hand.counts);

    auto label = std::string(name) + " / count";
    printRow(
        label.c_str(), segments, countInterpreted, countHand, footprint, counted);

    interpreted.sumForFill();
    hand.sumForFill();

    kernel.mode = fillMode;

    auto fillInterpreted = timeRuns([&] { interpreted.clearForFill(); },
                                    [&] { executor.dispatch(bindings, segments); });
    auto fillHand = timeRuns([&] { hand.clearForFill(); },
                             [&] { handBin(scene, hand, fillMode); });

    auto filled = sameWords(name, interpreted.counts, hand.counts)
                  && sameRuns(name, interpreted, hand);

    label = std::string(name) + " / fill";
    printRow(label.c_str(), segments, fillInterpreted, fillHand, footprint, filled);

    gSink += interpreted.counts.back() + hand.counts.back();
}

void benchBinning()
{
    std::printf("\nBinKernel over the PathBench scene (scale %.0f), per segment:\n",
                (double) pathScale);
    printHeader("path / pass", "segments");

    auto cases = pathBenchCases();
    auto whole = Scene {};

    for (const auto& item: cases)
    {
        auto scene = Scene {};
        gatherInto(scene, item.path);
        finish(scene);
        benchScene(item.name, scene);

        gatherInto(whole, item.path);
    }

    finish(whole);
    benchScene("all eight as one batch", whole);
}
#endif
} // namespace

// The stream kernels again at each batch width, one group per batch first,
// with no byte budget to narrow the widest.
void sweepBatchWidths()
{
    for (auto lanes: {64, 256, 512, 1024, 2048, 4096})
    {
        std::printf("\n");
        benchStreams(PlanOptions {lanes, std::numeric_limits<std::size_t>::max()});
    }
}

bool asksForSweep(int argc, char** argv)
{
    for (auto i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--sweep")
            return true;

    return false;
}

int main(int argc, char** argv)
{
#if !defined(NDEBUG)
    std::printf("Note: a Debug build -- the executor is always optimised, but "
                "build Release for representative numbers.\n\n");
#endif

    std::printf("eacp-cpu-compute benchmark | interpreter vs hand-written C++, "
                "median of runs\n\n");

    benchStreams(PlanOptions {});

    if (asksForSweep(argc, argv))
        sweepBatchWidths();

#if EACP_BENCH_PATHS
    benchBinning();
#else
    std::printf("\nBinKernel skipped: this build has no eacp-gpuwidgets.\n");
#endif

    std::printf("\nchecksum %llu\n", static_cast<unsigned long long>(gSink));

    if (gMismatches > 0)
    {
        std::printf("\n%d MISMATCH%s between the interpreter and the hand-written "
                    "loops\n",
                    gMismatches,
                    gMismatches == 1 ? "" : "ES");
        return 1;
    }

    return 0;
}
