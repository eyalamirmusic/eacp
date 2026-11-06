#pragma once
/** @file
    Watch sources, rebuild a module and reload it on every change; the layer under
    Reloadable.
*/

#include "../Process/BuildRunner.h"
#include "../Utils/SourceWatcher.h"
#include "HotReloadTarget.h"
#include "ReloadableLibrary.h"

namespace eacp::Plugins
{
// What a failed hot-reload build left behind.
struct BuildFailure
{
    std::string output;
    bool launched = false;
    bool exited = false;
    int exitCode = -1;

    // The line most likely to name the cause: a located compiler error, then
    // any error that is not the driver's, a CMake error, ninja's FAILED: line,
    // and the last line of output as the fallback.
    std::string summary;
};

// Builds a BuildFailure, summary included, from a finished build process.
BuildFailure buildFailureFrom(const Processes::ProcessResult& result);

// Watches a source tree and, on each settled burst of changes, runs the build
// command and reloads the module it produces. A failed build or load keeps the
// current generation. Nothing builds until the first change. Main thread only;
// every callback runs on the main thread and must not destroy the loop.
class HotReloadLoop
{
public:
    enum class State
    {
        Idle,
        Building,
        Loaded,
        BuildFailed,
        LoadFailed
    };

    struct Options
    {
        Files::SourceWatcher::Options sources;
        Processes::ProcessOptions buildCommand;
        FilePath modulePath;
        FilePath stagingDirectory = FilePath::appCacheDirectory() / "HotReload";
        // A rebuilt module that does not export this is refused. Empty checks
        // nothing.
        std::string requiredSymbol;
        // Returns a non-empty reason to refuse a rebuilt module.
        ReloadableLibrary::ValidateFunc validate = [](const DynamicLibrary&)
        { return std::string {}; };
    };

    explicit HotReloadLoop(const Options& options);
    HotReloadLoop(const HotReloadTarget& target, const std::string& requiredSymbol);

    HotReloadLoop(const HotReloadLoop&) = delete;
    HotReloadLoop& operator=(const HotReloadLoop&) = delete;
    HotReloadLoop(HotReloadLoop&&) = delete;
    HotReloadLoop& operator=(HotReloadLoop&&) = delete;

    // Builds and reloads now, without waiting for a change. During a build it
    // queues exactly one rebuild after the current one.
    void rebuild();

    [[nodiscard]] State getState() const;
    [[nodiscard]] std::string describeState() const;
    // 0 before the first successful load, one more per success.
    [[nodiscard]] int getGeneration() const;
    [[nodiscard]] ReloadableLibrary& getLibrary();
    [[nodiscard]] const FilePath& getSourceDirectory() const;
    [[nodiscard]] const Vector<FilePath>& getWatchedPaths() const;

    std::function<void()> onBuildStarted = [] {};
    // Each chunk of the build's merged stdout and stderr, as it arrives.
    std::function<void(std::string_view chunk)> onBuildOutput =
        [](std::string_view) {};
    std::function<void(ReloadableLibrary& library)> onReloaded =
        [](ReloadableLibrary&) {};
    std::function<void(const BuildFailure& failure)> onBuildFailed =
        [](const BuildFailure&) {};
    std::function<void(const std::string& error)> onLoadFailed =
        [](const std::string&) {};
    std::function<void()> onStateChanged = [] {};

private:
    Files::FileWatcher::ChangedFunc rebuildOnChange();
    void startBuilding();
    void buildFinished(const Processes::ProcessResult& result);
    void reload();
    void setState(State newState);

    ReloadableLibrary library;
    Processes::BuildRunner runner;
    State state = State::Idle;
    std::string failureMessage;
    Files::SourceWatcher watcher;
};

// The options a target's compile definitions describe: its source directory
// watched, its build command, its module, and no required symbol.
HotReloadLoop::Options hotReloadOptions(const HotReloadTarget& target);
} // namespace eacp::Plugins
