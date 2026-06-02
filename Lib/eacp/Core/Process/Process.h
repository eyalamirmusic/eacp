#pragma once

#include "../Threads/Async.h"
#include "../Utils/Common.h"

#include <string>
#include <vector>

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
    std::vector<std::string> arguments;
    std::string workingDirectory;
    std::vector<EnvironmentVariable> environment;
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
            const std::vector<std::string>& arguments = {});

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
                  const std::vector<std::string>& arguments = {});

// Same as run(), but on a background thread; the returned Async resolves on the
// main thread once the child exits.
Threads::Async<ProcessResult> runAsync(ProcessOptions options);
} // namespace eacp::Processes
