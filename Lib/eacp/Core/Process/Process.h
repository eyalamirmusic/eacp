#pragma once

#include "../Threads/Async.h"
#include "../Utils/Common.h"

namespace eacp::Processes
{
struct EnvironmentVariable
{
    std::string name;
    std::string value;
};

struct ProcessOptions
{
    std::string executable;
    Vector<std::string> arguments;
    std::string workingDirectory;
    Vector<EnvironmentVariable> environment;

    // When false the child inherits the launcher's stdio rather than having it
    // captured into output()/errorOutput(). Suits long-running children whose
    // output would otherwise buffer unbounded.
    bool captureOutput = true;

    // When true the child is launched detached: destroying the Process never
    // kills it, so it survives both the object and the launching process. For
    // hand-off launches, e.g. an updater starting its replacement before
    // exiting. Implies captureOutput = false; the launcher is expected to exit
    // soon after (a detached child that outlives a still-running launcher is
    // not reaped on POSIX until the launcher exits).
    bool detached = false;
};

struct ProcessResult
{
    bool launched = false;
    bool exited = false;
    int exitCode = -1;
    std::string output;
    std::string errorOutput;
};

// Launches and controls a single child process. stdout and stderr are captured
// in the background; stdin can be fed via writeToInput. The child is owned by
// this object: if it is still running when the Process is destroyed it is
// killed and reaped, so callers that want it to outlive the Process must wait()
// for it first.
class Process
{
public:
    explicit Process(ProcessOptions options);

    Process(const std::string& executable,
            const Vector<std::string>& arguments = {});

    ~Process();

    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    Process(Process&&) noexcept = default;

    [[nodiscard]] bool launched() const;
    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] long id() const;

    bool writeToInput(const std::string& data);
    void closeInput();

    int wait();
    void terminate();
    void kill();

    [[nodiscard]] std::string output() const;
    [[nodiscard]] std::string errorOutput() const;

private:
    struct Native;
    Pimpl<Native> impl;
};

// Convenience: launch, block until the child exits, and return everything it
// produced. Runs on the calling thread.
ProcessResult run(ProcessOptions options);
ProcessResult run(const std::string& executable,
                  const Vector<std::string>& arguments = {});

// Same as run(), but on a background thread; the returned Async resolves on the
// main thread once the child exits.
Threads::Async<ProcessResult> runAsync(ProcessOptions options);
} // namespace eacp::Processes
