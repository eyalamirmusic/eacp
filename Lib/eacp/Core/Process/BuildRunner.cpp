#include "BuildRunner.h"

#include "../Threads/EventLoop.h"

#include <atomic>
#include <mutex>
#include <string>
#include <utility>

namespace eacp::Processes
{
struct BuildRunner::Run
{
    explicit Run(BuildRunner& ownerToUse)
        : owner(ownerToUse)
    {
    }

    void track(Process* processToUse)
    {
        auto lock = std::lock_guard {mutex};
        process = processToUse;

        if (process != nullptr && cancelled)
            process->kill();
    }

    void cancel()
    {
        auto lock = std::lock_guard {mutex};
        cancelled = true;

        if (process != nullptr)
            process->kill();
    }

    bool wasCancelled()
    {
        auto lock = std::lock_guard {mutex};
        return cancelled;
    }

    BuildRunner& owner;
    std::atomic<bool> alive {true};
    std::mutex mutex;
    Process* process = nullptr;
    bool cancelled = false;
};

namespace
{
ProcessResult runToCompletion(const ProcessOptions& options,
                              const std::function<void(Process*)>& track)
{
    auto process = Process {options};

    auto result = ProcessResult {};
    result.launched = process.launched();

    if (!result.launched)
        return result;

    track(&process);
    process.closeInput();
    result.exitCode = process.wait();
    track(nullptr);

    result.exited = true;
    result.output = process.output();
    return result;
}
} // namespace

BuildRunner::BuildRunner(const ProcessOptions& commandToUse)
    : command(commandToUse)
{
    command.captureOutput = true;
    command.mergeErrorOutput = true;
    command.detached = false;
}

BuildRunner::~BuildRunner()
{
    if (current != nullptr)
        current->alive = false;

    cancel();
    joinWorker();
}

void BuildRunner::run()
{
    if (running)
    {
        rerunPending = true;
        return;
    }

    startRun();
}

void BuildRunner::cancel()
{
    rerunPending = false;

    if (running && current != nullptr)
        current->cancel();
}

bool BuildRunner::isRunning() const
{
    return running;
}

bool BuildRunner::isRerunPending() const
{
    return rerunPending;
}

void BuildRunner::startRun()
{
    joinWorker();

    auto run = std::make_shared<Run>(*this);
    current = run;
    running = true;

    auto options = command;

    auto forwardChunk = [run](std::string_view chunk)
    {
        auto text = std::string {chunk};

        auto deliver = [run, text]
        {
            if (run->alive)
                run->owner.onOutput(text);
        };

        Threads::callAsync(deliver);
    };

    options.onOutput = forwardChunk;

    auto work = [run, options]
    {
        auto track = [run](Process* process) { run->track(process); };
        auto result = runToCompletion(options, track);

        if (run->wasCancelled())
            result.exited = false;

        auto deliver = [run, result]
        {
            if (run->alive)
                run->owner.finish(run, result);
        };

        Threads::callAsync(deliver);
    };

    worker.start(work);
}

void BuildRunner::finish(const std::shared_ptr<Run>& finishedRun,
                         const ProcessResult& result)
{
    running = false;
    joinWorker();

    onFinished(result);

    if (!finishedRun->alive)
        return;

    auto rerun = std::exchange(rerunPending, false);

    if (rerun && !running)
        startRun();
}

void BuildRunner::joinWorker()
{
    worker.join();
}
} // namespace eacp::Processes
