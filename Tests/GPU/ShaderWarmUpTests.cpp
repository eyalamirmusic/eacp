#include "Common.h"

#include <eacp/GPU/Codegen/ShaderSets.h>

#include <atomic>

// ShaderWarmUp compiling on its worker into the device's ShaderLibraryCache,
// and the programs that find their libraries there afterwards. Every shader is
// a tint baked into the source as a literal, so each case compiles text no
// other case in the process has, and the cache's counters can be read as
// differences. Self-skips without a device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace ShaderWarmUpTests
{
struct WarmUpVertex
{
    float position[2];
};

constexpr WarmUpVertex warmUpTriangle[] = {
    {{-1.f, -1.f}}, {{3.f, -1.f}}, {{-1.f, 3.f}}};

struct TintShader final : ShaderProgram
{
    explicit TintShader(float tintToUse)
        : tint(tintToUse)
    {
        compile();
    }

    void define() override
    {
        auto position = vertexInput(&WarmUpVertex::position);
        setPosition(float4(position, 0.f, 1.f));
        setFragment(float4(constant(tint), 0.5f, 0.25f, 1.f));
    }

    float tint;

    EACP_SHADER()
};

struct ScaleKernel final : ComputeProgram
{
    explicit ScaleKernel(float factorToUse)
        : factor(factorToUse)
    {
        compile();
    }

    void define() override
    {
        auto i = threadId();
        write(output, i, input[i] * factor);
    }

    float factor;

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

struct TintView final : GPUView
{
    explicit TintView(float tint)
        : shader(tint)
    {
        setSampleCount(1);
        shader.setVertices(warmUpTriangle);
        shader.prepare(sampleCount());
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({Graphics::Color::black()});
        pass.draw(shader);
    }

    TintShader shader;
};

struct CacheDelta
{
    explicit CacheDelta(Device& deviceToUse = Device::shared())
        : cache(ShaderLibraryCache::of(deviceToUse))
        , before(cache.stats())
    {
    }

    int compiles() const { return cache.stats().compiles - before.compiles; }
    int hits() const { return cache.stats().hits - before.hits; }
    int waits() const { return cache.stats().waits - before.waits; }

    ShaderLibraryCache& cache;
    ShaderLibraryCache::Stats before;
};

bool hasDevice()
{
    return Device::shared().isValid();
}

void addTints(ShaderWarmUp& warmUp, float first, int count)
{
    for (auto index = 0; index < count; ++index)
        warmUp.add<TintShader>(first + 0.001f * (float) index);
}

ShaderWarmUp::Result finish(ShaderWarmUp& warmUp)
{
    return warmUp.start().waitFor(Time::MS {30000});
}

ShaderSource brokenSource()
{
    return nativeDialect(
        "this is not a shader", "this is not a shader", "this is not a shader");
}

bool waitUntilInFlight(const ShaderSource& source)
{
    auto& cache = ShaderLibraryCache::of(Device::shared());
    auto inFlight = [&] { return cache.isInFlight(source); };
    return Threads::runEventLoopUntil(inFlight, Time::MS {10000}, Time::MS {1});
}

EACP_SHADER_SET()
{
    visit<TintShader>(0.801f);
    visit<TintShader>(0.802f);
}

auto tCompletes = test("ShaderWarmUp/completesWithProgressReachingTheTotal") = []
{
    if (!hasDevice())
        return;

    auto seen = Vector<int> {};
    auto options = ShaderWarmUp::Options {};
    options.onProgress = [&seen](const ShaderWarmUp::Progress& progress)
    { seen.add(progress.completed); };

    auto warmUp = ShaderWarmUp {options};
    addTints(warmUp, 0.101f, 6);

    check(warmUp.progress().state == ShaderWarmUp::State::idle);

    const auto result = finish(warmUp);
    const auto progress = warmUp.progress();

    check(result.succeeded(),
          result.failures.empty() ? "" : result.failures[0].name);
    check(result.total == 6);
    check(result.compiled == 6);
    check(result.alreadyCached == 0);
    check(progress.state == ShaderWarmUp::State::done);
    check(progress.completed == 6 && progress.total == 6);
    check(progress.fraction == 1.f);
    check(progress.current.empty());

    check(!seen.empty());

    for (auto index = 1; index < seen.size(); ++index)
        check(seen[index] >= seen[index - 1]);

    if (!seen.empty())
        check(seen[seen.size() - 1] == 6);
};

auto tTimes = test("ShaderWarmUp/timesEveryShaderInTheOrderCompiled") = []
{
    if (!hasDevice())
        return;

    auto options = ShaderWarmUp::Options {};
    options.simulatedCompileTime = Time::MS {40};

    auto warmUp = ShaderWarmUp {options};
    warmUp.addNamed<TintShader>("first", 0.151f);
    warmUp.addNamed<TintShader>("second", 0.152f);
    warmUp.addNamed<TintShader>("third", 0.153f);

    const auto result = finish(warmUp);

    check(result.succeeded());
    check(result.timings.size() == 3);

    if (result.timings.size() != 3)
        return;

    check(result.timings[0].name == "first");
    check(result.timings[1].name == "second");
    check(result.timings[2].name == "third");

    for (const auto& timing: result.timings)
        check(timing.compile >= Time::MS {40});

    check(result.recording <= result.elapsed);
    check(result.elapsed >= Time::MS {120});
};

auto tPrepareHits = test("ShaderWarmUp/aPrepareAfterwardsFindsTheLibrary") = []
{
    if (!hasDevice())
        return;

    auto warmUp = ShaderWarmUp {};
    warmUp.add<TintShader>(0.201f);
    finish(warmUp);

    const auto delta = CacheDelta {};
    auto shader = TintShader {0.201f};
    shader.setVertices(warmUpTriangle);
    shader.prepare(1);

    check(shader.pipeline().isValid());
    check(delta.compiles() == 0);
    check(delta.hits() == 1);
};

RenderPipelineDescriptor tintPipeline()
{
    auto descriptor = RenderPipelineDescriptor {};
    descriptor.sampleCount = 1;
    return descriptor;
}

auto tBuildsPipelinesOnMetal =
    test("ShaderWarmUp/buildsPipelinesOnTheWorkerOnlyOnMetal") = []
{
    auto warmUp = ShaderWarmUp {};
    const auto isMetal = detail::nativeShaderBackend() == ShaderBackend::Metal;

    check(warmUp.buildsPipelines() == isMetal);

    auto device = Device {};
    auto onItsOwnDevice = ShaderWarmUp {device};

    check(!onItsOwnDevice.buildsPipelines());
};

auto tWithPipeline =
    test("ShaderWarmUp/aShaderAddedWithItsPipelineIsPreparedFromTheCache") = []
{
    if (!hasDevice())
        return;

    auto warmUp = ShaderWarmUp {};
    warmUp.addWithPipeline<TintShader>(tintPipeline(), 0.951f)
        .addNamedWithPipeline<TintShader>("second", tintPipeline(), 0.952f);
    const auto result = finish(warmUp);

    check(result.succeeded());
    check(result.compiled == 2);
    check(result.pipelines == (warmUp.buildsPipelines() ? 2 : 0));
    check(result.timings.size() == 2);

    const auto delta = CacheDelta {};
    auto view = TintView {0.951f};
    view.setBounds({0.f, 0.f, 16.f, 16.f});

    check(view.shader.pipeline().isValid());
    check(delta.compiles() == 0);
    check(delta.hits() == 1);
    check(view.renderToImage(1.f).isValid());
};

auto tWithPipelineOnItsOwnDevice =
    test("ShaderWarmUp/onItsOwnDeviceAShaderWithAPipelineOnlyCompiles") = []
{
    auto device = Device {};

    if (!device.isValid())
        return;

    auto warmUp = ShaderWarmUp {device};
    warmUp.addWithPipeline<TintShader>(tintPipeline(), 0.961f);
    const auto result = finish(warmUp);

    check(result.succeeded());
    check(result.compiled == 1);
    check(result.pipelines == 0);
    check(ShaderLibraryCache::of(device).contains(TintShader {0.961f}.source()));
};

auto tKernelOnItsOwnDevice =
    test("ShaderWarmUp/aKernelOnItsOwnDeviceFindsTheLibrary") = []
{
    auto device = Device {};

    if (!device.isValid())
        return;

    auto warmUp = ShaderWarmUp {device};
    warmUp.add<ScaleKernel>(2.5f);
    const auto result = finish(warmUp);

    check(result.compiled == 1);

    const auto delta = CacheDelta {device};
    const auto values = Vector<float> {1.f, 2.f, 3.f, 4.f};
    const auto bytes = (std::int64_t) values.size() * (std::int64_t) sizeof(float);

    auto input = device.makeBuffer(values.data(), bytes, BufferUsage::Storage);
    auto output = device.makeBuffer(bytes, BufferUsage::Storage);

    auto kernel = ScaleKernel {2.5f};
    kernel.input = input;
    kernel.output = output;
    kernel.prepare(device);

    check(kernel.isValid());
    check(delta.compiles() == 0);
    check(delta.hits() == 1);

    if (!kernel.isValid())
        return;

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, values.size());
    }

    commands.commit();

    auto results = Vector<float>(values.size());
    output.read(results.data(), bytes);

    for (auto index = 0; index < values.size(); ++index)
        check(results[index] == values[index] * 2.5f);
};

// A fresh cache has followed no backend generation, so a warm() before
// followBackendGeneration would be dropped by the first acquire on every
// platform, not only where the generation starts above 0.
auto tWarmFollowsTheDevice =
    test("ShaderLibraryCache/aWarmAfterFollowingTheDeviceIsFoundByAcquire") = []
{
    auto device = Device {};

    if (!device.isValid())
        return;

    auto& cache = ShaderLibraryCache::of(device);
    const auto source = ScaleKernel {3.5f}.source();

    cache.followBackendGeneration(device);
    const auto warmed = cache.warm(device, source);

    check(warmed.valid);
    check(warmed.compiledNow);

    const auto delta = CacheDelta {device};
    const auto library = cache.acquire(device, source);

    check(library->isValid());
    check(delta.compiles() == 0);
    check(delta.hits() == 1);
};

auto tCompileOnce =
    test("ShaderLibraryCache/twoInstancesAndTwoPreparesCompileOnce") = []
{
    if (!hasDevice())
        return;

    const auto delta = CacheDelta {};

    auto first = TintShader {0.301f};
    first.setVertices(warmUpTriangle);
    first.prepare(1);
    first.prepare(1);

    auto second = TintShader {0.301f};
    second.setVertices(warmUpTriangle);
    second.prepare(1);

    check(first.pipeline().isValid());
    check(second.pipeline().isValid());
    check(delta.compiles() == 1);
    check(delta.hits() == 2);
};

auto tWaits = test("ShaderWarmUp/aPrepareWaitsForTheShaderInFlight") = []
{
    if (!hasDevice())
        return;

    auto options = ShaderWarmUp::Options {};
    options.simulatedCompileTime = Time::MS {300};

    auto warmUp = ShaderWarmUp {options};
    warmUp.add<TintShader>(0.401f);

    const auto delta = CacheDelta {};
    auto shader = TintShader {0.401f};
    auto done = warmUp.start();

    check(waitUntilInFlight(shader.source()));

    shader.setVertices(warmUpTriangle);
    shader.prepare(1);

    check(shader.pipeline().isValid());
    check(delta.compiles() == 1);
    check(delta.waits() == 1);

    const auto result = done.waitFor(Time::MS {30000});
    check(result.compiled == 1);
};

auto tCancel = test("ShaderWarmUp/cancelStopsBetweenShaders") = []
{
    if (!hasDevice())
        return;

    auto options = ShaderWarmUp::Options {};
    options.simulatedCompileTime = Time::MS {150};

    auto warmUp = ShaderWarmUp {options};
    addTints(warmUp, 0.501f, 6);

    auto done = warmUp.start();
    check(waitUntilInFlight(TintShader {0.501f}.source()));

    warmUp.cancel();
    const auto result = done.waitFor(Time::MS {30000});
    const auto progress = warmUp.progress();

    check(result.cancelled);
    check(result.total == 6);
    check(result.compiled >= 1 && result.compiled < 6);
    check(progress.state == ShaderWarmUp::State::cancelled);
    check(progress.completed < progress.total);

    auto& cache = ShaderLibraryCache::of(Device::shared());
    check(cache.contains(TintShader {0.501f}.source()));
    check(!cache.contains(TintShader {0.505f}.source()));
};

auto tDestroyMidRun = test("ShaderWarmUp/destroyingItMidRunNeverCallsBack") = []
{
    if (!hasDevice())
        return;

    auto callbacks = std::make_shared<std::atomic<int>>(0);
    auto resolved = std::make_shared<std::atomic<bool>>(false);

    auto options = ShaderWarmUp::Options {};
    options.simulatedCompileTime = Time::MS {200};
    options.onProgress = [callbacks](const ShaderWarmUp::Progress&)
    { ++*callbacks; };

    auto warmUp = makeOwned<ShaderWarmUp>(options);
    addTints(*warmUp, 0.601f, 4);

    auto markResolved = [resolved](const ShaderWarmUp::Result&)
    { *resolved = true; };
    warmUp->start().then(markResolved);

    check(waitUntilInFlight(TintShader {0.601f}.source()));

    warmUp.reset();
    const auto atDestruction = callbacks->load();

    Threads::runEventLoopFor(Time::MS {500});

    check(callbacks->load() == atDestruction);
    check(!*resolved);
    check(!ShaderLibraryCache::of(Device::shared())
               .isInFlight(TintShader {0.601f}.source()));
};

auto tFailure = test("ShaderWarmUp/aFailingShaderIsReportedAndTheRestCompile") = []
{
    if (!hasDevice())
        return;

    auto warmUp = ShaderWarmUp {};
    warmUp.add<TintShader>(0.701f);
    warmUp.add("broken", brokenSource());
    warmUp.add<TintShader>(0.702f);

    const auto result = finish(warmUp);

    check(result.total == 3);
    check(!result.succeeded());
    check(result.failures.size() == 1);

    if (result.failures.size() == 1)
    {
        check(result.failures[0].name == "broken");
        check(result.failures[0].compilation.hasErrors(),
              result.failures[0].compilation.describe());
    }

    check(warmUp.progress().state == ShaderWarmUp::State::failed);
    check(warmUp.progress().completed == 3);

    auto& cache = ShaderLibraryCache::of(Device::shared());
    check(cache.contains(TintShader {0.701f}.source()));
    check(cache.contains(TintShader {0.702f}.source()));

    const auto delta = CacheDelta {};
    const auto again = cache.acquire(Device::shared(), brokenSource());

    check(!again->isValid());
    check(again->compilationInfo().hasErrors());
    check(delta.compiles() == 1);
};

auto tSets = test("ShaderWarmUp/aShaderSetIsRecordedOnTheWorker") = []
{
    if (!hasDevice())
        return;

    auto warmUp = ShaderWarmUp {};
    warmUp.addRegisteredSets();

    const auto result = finish(warmUp);

    check(result.succeeded());
    check(result.total >= 2);

    const auto delta = CacheDelta {};
    auto first = TintShader {0.801f};
    first.setVertices(warmUpTriangle);
    first.prepare(1);
    auto second = TintShader {0.802f};
    second.setVertices(warmUpTriangle);
    second.prepare(1);

    check(delta.compiles() == 0);
    check(delta.hits() == 2);
};

auto tRecordingThrows = test("ShaderWarmUp/aRecordingThatThrowsIsAFailure") = []
{
    if (!hasDevice())
        return;

    auto warmUp = ShaderWarmUp {};
    auto throwing = [](const ShaderWarmUp::SourceSink&)
    { throw std::runtime_error("no shader here"); };
    warmUp.addRecipe(throwing);
    warmUp.add<TintShader>(0.851f);

    const auto result = finish(warmUp);

    check(result.total == 1);
    check(result.compiled == 1);
    check(result.failures.size() == 1);
};

auto tSecondStartRefused =
    test("ShaderWarmUp/aSecondStartWhileRunningIsRejected") = []
{
    if (!hasDevice())
        return;

    auto options = ShaderWarmUp::Options {};
    options.simulatedCompileTime = Time::MS {100};

    auto warmUp = ShaderWarmUp {options};
    warmUp.add<TintShader>(0.871f);

    auto first = warmUp.start();
    auto second = warmUp.start();

    check(second.isRejected());

    const auto result = first.waitFor(Time::MS {30000});
    check(result.compiled == 1);

    const auto again = finish(warmUp);
    check(again.compiled == 0);
    check(again.alreadyCached == 1);
};

auto tRendersMeanwhile =
    test("ShaderWarmUp/theMainThreadRendersWhileItCompiles") = []
{
    if (!hasDevice())
        return;

    auto view = TintView {0.901f};
    view.setBounds({0.f, 0.f, 16.f, 16.f});

    auto warmUp = ShaderWarmUp {};
    addTints(warmUp, 0.911f, 12);
    auto done = warmUp.start();

    auto frames = 0;
    auto lastFrameValid = true;

    while (!done.isReady() && frames < 2000)
    {
        lastFrameValid = view.renderToImage(1.f).isValid() && lastFrameValid;
        ++frames;
        Threads::runEventLoopFor(Time::MS {1});
    }

    const auto result = done.waitFor(Time::MS {30000});

    check(result.succeeded());
    check(result.compiled + result.alreadyCached == 12);
    check(frames > 0);
    check(lastFrameValid);
};
} // namespace ShaderWarmUpTests
