#pragma once

#include "../Threads/Thread.h"
#include "Process.h"

#include <functional>
#include <memory>
#include <string_view>

namespace eacp::Processes
{
// Runs one command again on demand, on a background thread. The command runs
// with its stderr merged into its stdout; onOutput and onFinished are called
// on the main thread, every chunk of a run before its onFinished. A run()
// while a run is in flight asks for exactly one more run once it finishes.
// Main thread only.
class BuildRunner
{
public:
    explicit BuildRunner(const ProcessOptions& commandToUse);
    ~BuildRunner();

    BuildRunner(const BuildRunner&) = delete;
    BuildRunner& operator=(const BuildRunner&) = delete;
    BuildRunner(BuildRunner&&) = delete;
    BuildRunner& operator=(BuildRunner&&) = delete;

    void run();

    // A cancelled run still finishes, with ProcessResult::exited false.
    void cancel();

    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] bool isRerunPending() const;

    std::function<void(std::string_view chunk)> onOutput = [](std::string_view) {};
    std::function<void(const ProcessResult& result)> onFinished =
        [](const ProcessResult&) {};

private:
    struct Run;

    void startRun();
    void finish(const std::shared_ptr<Run>& finishedRun,
                const ProcessResult& result);
    void joinWorker();

    ProcessOptions command;
    std::shared_ptr<Run> current;
    Threads::Thread worker;
    bool running = false;
    bool rerunPending = false;
};
} // namespace eacp::Processes
