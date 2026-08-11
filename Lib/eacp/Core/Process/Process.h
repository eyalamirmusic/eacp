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

    // When false the child inherits the launcher's stdio instead of having it
    // captured into output()/errorOutput(), which buffers unbounded.
    bool captureOutput = true;

    // Destroying the Process never kills a detached child, so it survives both
    // the object and the launcher. Implies captureOutput = false, and on POSIX
    // it is not reaped until the launcher exits.
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

// Launches and controls a single child process, capturing stdout and stderr in
// the background. The child is owned by this object: still running at
// destruction, it is killed and reaped, so wait() first if it must outlive it.
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

// Blocks the calling thread until the child exits.
ProcessResult run(ProcessOptions options);
ProcessResult run(const std::string& executable,
                  const Vector<std::string>& arguments = {});

// Runs on a background thread; the Async resolves on the main thread.
Threads::Async<ProcessResult> runAsync(ProcessOptions options);
} // namespace eacp::Processes
