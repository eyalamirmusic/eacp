#pragma once

#include "../Shader/CompilationInfo.h"
#include "../Shader/ShaderSource.h"
#include "Forward.h"

#include <eacp/Core/Threads/Async.h>
#include <eacp/Core/Threads/Thread.h>

#include <concepts>
#include <typeinfo>

namespace eacp::GPU
{
class Device;
struct ShaderSetDefinition;

template <typename Shader>
concept HasShaderSource = requires(const Shader& shader) {
    { shader.source() } -> std::convertible_to<const ShaderSource&>;
};

// Compiles a set of shaders on one worker thread, ahead of the programs that
// will draw with them: each library lands in the device's ShaderLibraryCache,
// so a GPUView created once start() resolves prepares its programs with no
// compile left to do (only their pipelines, which are still built on the main
// thread). The main thread meanwhile keeps running and can draw progress().
//
//   auto warmUp = GPU::ShaderWarmUp {};
//   warmUp.add<SkyShader>().add<GainKernel>().add<SpriteShader>(nearest);
//   warmUp.addRegisteredSets();                  // every EACP_SHADER_SET
//   warmUp.start().then([this](const auto& result) { showTheRealView(); });
//
// The shaders are recorded on the worker too - a shader added by type is
// constructed there - so a shader's constructor must not touch a device:
// compile() from it is fine, setVertices() is not. Everything added is run in
// the order added. Add before start(); destroy on the main thread, which
// cancels, waits for the shader in flight and never calls back afterwards.
class ShaderWarmUp
{
public:
    enum class State
    {
        idle,
        recording,
        compiling,
        done,
        cancelled,
        failed
    };

    struct Progress
    {
        bool isFinished() const;

        State state = State::idle;

        // Shaders finished, cached or failed, out of total. The total is 0
        // until recording is over, when every shader added is known.
        int completed = 0;
        int total = 0;

        // completed over total: 0 while recording, 1 once done.
        float fraction = 0.f;

        // The shader being compiled, empty between shaders.
        std::string current;
    };

    struct Failure
    {
        std::string name;
        CompilationInfo compilation;
    };

    struct Timing
    {
        std::string name;
        Time::MS compile {0};
    };

    struct Result
    {
        bool succeeded() const;

        // Every shader recorded.
        int total = 0;

        // Compiled, or loaded from a precompiled blob, by this warm-up.
        int compiled = 0;

        // In the cache already, or compiled meanwhile by another thread.
        int alreadyCached = 0;

        // Of the valid ones, how many came from a precompiled blob.
        int precompiled = 0;

        // Each shader that did not compile, with what the compiler said, and
        // each recording that threw.
        Vector<Failure> failures;

        // How long each shader took to compile or to find in the cache, in the
        // order compiled, simulatedCompileTime included.
        Vector<Timing> timings;

        bool cancelled = false;
        Time::MS elapsed {0};

        // Of elapsed, the part spent recording and emitting every shader
        // before the first compile.
        Time::MS recording {0};
    };

    struct Options
    {
        // On the main thread after each shader, and once more at the end.
        std::function<void(const Progress&)> onProgress = [](const Progress&) {};

        // For debugging a progress screen: each shader is held in flight this
        // much longer before it compiles, its cache entry claimed, so the
        // percentage can be seen moving. cancel() cuts the wait short.
        Time::MS simulatedCompileTime {0};
    };

    // Called on the worker with each shader a recipe records.
    using SourceSink =
        std::function<void(const std::string& name, const ShaderSource& source)>;
    using Recipe = std::function<void(const SourceSink& sink)>;
    using GraphSource = std::function<void(const ShaderGraphVisitor& visitor)>;

    // On Device::shared(), or on `device`, which has to outlive this.
    ShaderWarmUp();
    explicit ShaderWarmUp(const Options& optionsToUse);
    explicit ShaderWarmUp(Device& deviceToUse);
    ShaderWarmUp(Device& deviceToUse, const Options& optionsToUse);
    ~ShaderWarmUp();

    ShaderWarmUp(const ShaderWarmUp&) = delete;
    ShaderWarmUp& operator=(const ShaderWarmUp&) = delete;

    // A ShaderProgram or ComputeProgram type, built on the worker from a copy
    // of `args`, once per variant the app draws with. Progress names it by its
    // type, or by `name` through addNamed.
    template <HasShaderSource Shader, typename... Args>
    ShaderWarmUp& add(const Args&... args)
    {
        return addNamed<Shader>(typeNameOf(typeid(Shader)), args...);
    }

    template <HasShaderSource Shader, typename... Args>
    ShaderWarmUp& addNamed(const std::string& name, const Args&... args)
    {
        auto record = [name, args...](const SourceSink& sink)
        { sink(name, static_cast<const ShaderSource&>(Shader(args...).source())); };

        return addRecipe(record);
    }

    // A source already emitted, by a program constructed on this thread.
    ShaderWarmUp& add(const std::string& name, const ShaderSource& source);

    // Every graph a set visits. The set is run on the worker: do not run the
    // same set anywhere else meanwhile (ShaderSetDefinition::run swaps its
    // visitor).
    ShaderWarmUp& add(const ShaderSetDefinition& set,
                      const std::string& name = "set");

    // Every EACP_SHADER_SET linked into the app.
    ShaderWarmUp& addRegisteredSets();

    // A module's own shaders through its hook, such as
    // addGraphs("layers", &UI::LayerRenderer::forEachShaderGraph).
    ShaderWarmUp& addGraphs(const std::string& name, const GraphSource& graphs);

    // Anything else that can hand over sources.
    ShaderWarmUp& addRecipe(const Recipe& recipe);

    // Starts the worker. Resolves on the main thread, once, unless this object
    // is destroyed first; while one is running a second start() is rejected.
    Threads::Async<Result> start();

    // Stops between shaders: the one in flight finishes and is cached, the rest
    // are not compiled, and the result says cancelled.
    void cancel();

    bool isRunning() const;

    // From any thread.
    Progress progress() const;

    Device& device() const;

private:
    static std::string typeNameOf(const std::type_info& type);

    struct Job;
    struct Run;

    Device& target;
    Options options;
    Vector<Recipe> recipes;
    std::shared_ptr<Job> job;
    Threads::Thread worker {Threads::ThreadOptions {.name = "EACP Shader WarmUp"}};
};
} // namespace eacp::GPU
