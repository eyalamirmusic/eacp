#pragma once

#include "../Process/BuildRunner.h"
#include "../Utils/SourceWatcher.h"
#include "HotReloadTarget.h"
#include "ReloadableLibrary.h"

namespace eacp::Plugins
{
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
        std::string requiredSymbol;
        ReloadableLibrary::ValidateFunc validate = [](const DynamicLibrary&)
        { return std::string {}; };
    };

    explicit HotReloadLoop(const Options& options);
    HotReloadLoop(const HotReloadTarget& target, const std::string& requiredSymbol);

    HotReloadLoop(const HotReloadLoop&) = delete;
    HotReloadLoop& operator=(const HotReloadLoop&) = delete;
    HotReloadLoop(HotReloadLoop&&) = delete;
    HotReloadLoop& operator=(HotReloadLoop&&) = delete;

    void rebuild();

    [[nodiscard]] State getState() const;
    [[nodiscard]] std::string describeState() const;
    [[nodiscard]] int getGeneration() const;
    [[nodiscard]] ReloadableLibrary& getLibrary();
    [[nodiscard]] const FilePath& getSourceDirectory() const;
    [[nodiscard]] const Vector<FilePath>& getWatchedPaths() const;

    std::function<void()> onBuildStarted = [] {};
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
