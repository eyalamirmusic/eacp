#include "ShaderWarmUp.h"

#include "../Device/Device.h"
#include "../Shader/Dxc.h"
#include "../Shader/ShaderLibraryCache.h"
#include "ShaderBuilder.h"
#include "ShaderGraph.h"
#include "ShaderSets.h"

#include <eacp/Core/Utils/Strings.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>

#ifndef _MSC_VER
#include <cxxabi.h>
#endif

#include <cstdlib>

namespace eacp::GPU
{
namespace
{
struct NamedSource
{
    std::string name;
    ShaderSource source;
};

std::string
    describeGraph(const std::string& name, int index, const ShaderGraph& graph)
{
    return Strings::concat(name,
                           " #",
                           index + 1,
                           " (",
                           graph.isCompute() ? "compute kernel" : "render shader",
                           ")");
}

ShaderSource nativeSourceOf(const ShaderGraph& graph)
{
    return buildShader(graph, detail::nativeShaderBackend()).source;
}

ShaderWarmUp::Recipe graphRecipe(const std::string& name,
                                 const ShaderWarmUp::GraphSource& graphs)
{
    return [name, graphs](const ShaderWarmUp::SourceSink& sink)
    {
        auto index = 0;
        auto visitor = ShaderGraphVisitor {
            [&](const ShaderGraph& graph)
            {
                sink(describeGraph(name, index, graph), nativeSourceOf(graph));
                ++index;
            }};
        graphs(visitor);
    };
}

std::string describeException(const std::exception_ptr& thrown)
{
    try
    {
        std::rethrow_exception(thrown);
    }
    catch (const std::exception& error)
    {
        return error.what();
    }
    catch (...)
    {
        return "an exception that is not a std::exception";
    }
}

ShaderWarmUp::Failure recordingFailure(int recipe, const std::exception_ptr& thrown)
{
    auto failure = ShaderWarmUp::Failure {};
    failure.name = Strings::concat("recording #", recipe + 1);
    failure.compilation.add(
        CompilationMessageType::Error,
        Strings::concat("recording the shader threw: ", describeException(thrown)));
    return failure;
}

void loadDxcBeforeTheWorker()
{
    if (Dxc::isEnabled())
        Dxc::load();
}

Time::MS millisecondsSince(std::chrono::steady_clock::time_point started)
{
    const auto elapsed = std::chrono::steady_clock::now() - started;
    return Time::MS {
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()};
}
} // namespace

bool ShaderWarmUp::Progress::isFinished() const
{
    return state == State::done || state == State::cancelled
           || state == State::failed;
}

bool ShaderWarmUp::Result::succeeded() const
{
    return !cancelled && failures.empty();
}

struct ShaderWarmUp::Job
{
    Progress progress() const
    {
        auto result = Progress {};
        result.state = state;
        result.completed = completed;
        result.total = total;

        if (result.state == State::done)
            result.fraction = 1.f;
        else if (result.total > 0)
            result.fraction = (float) result.completed / (float) result.total;

        auto lock = std::lock_guard {mutex};
        result.current = current;
        return result;
    }

    void setCurrent(const std::string& name)
    {
        auto lock = std::lock_guard {mutex};
        current = name;
    }

    void requestCancel()
    {
        auto lock = std::lock_guard {mutex};
        cancelRequested = true;
        wake.notify_all();
    }

    void waitUnlessCancelled(Time::MS duration)
    {
        if (duration.count <= 0)
            return;

        auto lock = std::unique_lock {mutex};
        auto isCancelled = [this] { return cancelRequested.load(); };
        wake.wait_for(lock, std::chrono::milliseconds(duration.count), isCancelled);
    }

    void announceProgress(const std::shared_ptr<Job>& self)
    {
        auto deliver = [self]
        {
            if (!self->abandoned)
                self->onProgress(self->progress());
        };

        Threads::callAsync(deliver);
    }

    void finish(const std::shared_ptr<Job>& self, const Result& result)
    {
        auto deliver = [self, result]
        {
            if (self->abandoned)
                return;

            self->onProgress(self->progress());
            self->promise.resolve(result);
        };

        Threads::callAsync(deliver);
    }

    std::atomic<State> state {State::idle};
    std::atomic<int> completed {0};
    std::atomic<int> total {0};
    std::atomic<bool> cancelRequested {false};
    std::atomic<bool> abandoned {false};

    mutable std::mutex mutex;
    std::condition_variable wake;
    std::string current;

    std::function<void(const Progress&)> onProgress = [](const Progress&) {};
    Threads::AsyncPromise<Result> promise;
};

struct ShaderWarmUp::Run
{
    void operator()() const
    {
        const auto started = std::chrono::steady_clock::now();
        auto result = ShaderWarmUp::Result {};
        const auto sources = record(result);
        result.recording = millisecondsSince(started);

        job->total = sources.size();
        job->state = State::compiling;
        result.total = sources.size();

        compile(sources, result);

        result.elapsed = millisecondsSince(started);
        result.cancelled = job->cancelRequested && job->completed < job->total;

        job->setCurrent({});
        job->state = result.cancelled          ? State::cancelled
                     : result.failures.empty() ? State::done
                                               : State::failed;
        job->finish(job, result);
    }

    Vector<NamedSource> record(ShaderWarmUp::Result& result) const
    {
        job->state = State::recording;
        auto sources = Vector<NamedSource> {};

        auto sink = ShaderWarmUp::SourceSink {
            [&sources](const std::string& name, const ShaderSource& source)
            { sources.add(NamedSource {name, source}); }};

        for (auto index = 0; index < recipes.size(); ++index)
        {
            if (job->cancelRequested)
                break;

            try
            {
                recipes[index](sink);
            }
            catch (...)
            {
                result.failures.add(
                    recordingFailure(index, std::current_exception()));
            }
        }

        return sources;
    }

    void compile(const Vector<NamedSource>& sources,
                 ShaderWarmUp::Result& result) const
    {
        auto& cache = ShaderLibraryCache::of(*device);
        auto hold = [this] { job->waitUnlessCancelled(simulatedCompileTime); };

        for (const auto& shader: sources)
        {
            if (job->cancelRequested)
                break;

            job->setCurrent(shader.name);

            const auto compileStarted = std::chrono::steady_clock::now();
            const auto warmed = cache.warm(*device, shader.source, hold);
            result.timings.add(
                Timing {shader.name, millisecondsSince(compileStarted)});
            count(shader, warmed, result);

            ++job->completed;
            job->announceProgress(job);
        }
    }

    static void count(const NamedSource& shader,
                      const ShaderLibraryCache::Warmed& warmed,
                      ShaderWarmUp::Result& result)
    {
        if (warmed.compiledNow)
            ++result.compiled;
        else
            ++result.alreadyCached;

        if (warmed.valid && warmed.precompiled)
            ++result.precompiled;

        if (!warmed.valid)
            result.failures.add(
                ShaderWarmUp::Failure {shader.name, warmed.compilation});
    }

    std::shared_ptr<ShaderWarmUp::Job> job;
    Vector<ShaderWarmUp::Recipe> recipes;
    Device* device = nullptr;
    Time::MS simulatedCompileTime {0};
};

ShaderWarmUp::ShaderWarmUp()
    : ShaderWarmUp(Device::shared(), Options {})
{
}

ShaderWarmUp::ShaderWarmUp(const Options& optionsToUse)
    : ShaderWarmUp(Device::shared(), optionsToUse)
{
}

ShaderWarmUp::ShaderWarmUp(Device& deviceToUse)
    : ShaderWarmUp(deviceToUse, Options {})
{
}

ShaderWarmUp::ShaderWarmUp(Device& deviceToUse, const Options& optionsToUse)
    : target(deviceToUse)
    , options(optionsToUse)
    , job(std::make_shared<Job>())
{
}

ShaderWarmUp::~ShaderWarmUp()
{
    job->requestCancel();
    worker.join();
    job->abandoned = true;
    job->promise.abandon();
}

ShaderWarmUp& ShaderWarmUp::add(const std::string& name, const ShaderSource& source)
{
    auto record = [name, source](const SourceSink& sink) { sink(name, source); };
    return addRecipe(record);
}

ShaderWarmUp& ShaderWarmUp::add(const ShaderSetDefinition& set,
                                const std::string& name)
{
    auto graphs = [&set](const ShaderGraphVisitor& visitor) { set.run(visitor); };
    return addGraphs(name, graphs);
}

ShaderWarmUp& ShaderWarmUp::addRegisteredSets()
{
    const auto& sets = ShaderSets::registered();

    for (auto index = 0; index < sets.size(); ++index)
        add(*sets[index], Strings::concat("shader set ", index + 1));

    return *this;
}

ShaderWarmUp& ShaderWarmUp::addGraphs(const std::string& name,
                                      const GraphSource& graphs)
{
    return addRecipe(graphRecipe(name, graphs));
}

ShaderWarmUp& ShaderWarmUp::addRecipe(const Recipe& recipe)
{
    recipes.add(recipe);
    return *this;
}

Threads::Async<ShaderWarmUp::Result> ShaderWarmUp::start()
{
    if (isRunning())
    {
        auto refused = Threads::AsyncPromise<Result> {};
        refused.reject("the shader warm-up is already running");
        return refused.get();
    }

    worker.join();
    job->abandoned = true;
    job->promise.abandon();

    job = std::make_shared<Job>();
    job->onProgress = options.onProgress;
    job->state = State::recording;

    loadDxcBeforeTheWorker();
    ShaderLibraryCache::of(target).followBackendGeneration(target);

    auto run = Run {job, recipes, &target, options.simulatedCompileTime};
    worker.start(run);

    return job->promise.get();
}

void ShaderWarmUp::cancel()
{
    job->requestCancel();
}

bool ShaderWarmUp::isRunning() const
{
    const auto state = job->state.load();
    return state == State::recording || state == State::compiling;
}

ShaderWarmUp::Progress ShaderWarmUp::progress() const
{
    return job->progress();
}

Device& ShaderWarmUp::device() const
{
    return target;
}

std::string ShaderWarmUp::typeNameOf(const std::type_info& type)
{
#ifndef _MSC_VER
    auto status = 0;
    auto* demangled = abi::__cxa_demangle(type.name(), nullptr, nullptr, &status);

    if (status == 0 && demangled != nullptr)
    {
        auto name = std::string {demangled};
        std::free(demangled);
        return name;
    }

    std::free(demangled);
    return type.name();
#else
    auto name = std::string_view {type.name()};

    for (auto keyword: {std::string_view {"struct "}, std::string_view {"class "}})
        if (name.starts_with(keyword))
            name.remove_prefix(keyword.size());

    return std::string {name};
#endif
}
} // namespace eacp::GPU
