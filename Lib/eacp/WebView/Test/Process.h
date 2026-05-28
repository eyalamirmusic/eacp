#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace eacp::WebView::Test
{

struct ProcessOptions
{
    // argv[1..n]. argv[0] is set to the executable path by Process.
    std::vector<std::string> args;

    // Env vars merged onto the inherited parent environment. An empty
    // value removes the variable from the child's env.
    std::map<std::string, std::string> env;

    // Working directory for the child. Empty -> inherit parent's cwd.
    std::string workingDirectory;
};

// RAII handle to a spawned child process with its stdout/stderr piped
// back to the parent. The dtor terminates the child if it is still
// running so callers don't have to think about leaks on the
// exceptional path.
//
// Output is collected by two background reader threads (one per
// pipe). Callers see a growing string buffer; pollOutput() blocks for
// up to `timeout` waiting for at least one new byte (or EOF) to
// arrive, then returns.
class Process
{
public:
    Process(const std::string& executable, const ProcessOptions& options);
    ~Process();

    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    Process(Process&&) = delete;
    Process& operator=(Process&&) = delete;

    // Block up to `timeout` waiting for new output on either stream
    // (or for the child to exit and both streams to hit EOF). Returns
    // the number of new bytes appended across both buffers; 0 means
    // nothing arrived in the window.
    std::size_t pollOutput(std::chrono::milliseconds timeout);

    // Snapshots of the accumulated buffers. Safe to call from any
    // thread; the reader threads append, callers read.
    std::string stdoutBuffer() const;
    std::string stderrBuffer() const;

    // True when the child has exited AND both stdout / stderr readers
    // have observed EOF (so the buffers are fully drained).
    bool hasExited();

    // Polite stop, escalating to a hard kill if the child does not
    // exit within `gracePeriod`. Apple: SIGTERM -> SIGKILL. Windows:
    // TerminateProcess (no portable polite signal for a non-console
    // GUI child; the WebView test child has no persistent state to
    // flush). Blocks until the child is reaped.
    void terminate(std::chrono::milliseconds gracePeriod
                   = std::chrono::milliseconds {2000});

    // Set only after hasExited() returns true. exitCode() is the
    // child's exit status; terminatingSignal() is the POSIX signal
    // that killed it (0 if it exited normally; always 0 on Windows).
    int exitCode() const;
    int terminatingSignal() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace eacp::WebView::Test
