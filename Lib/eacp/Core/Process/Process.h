#pragma once

#include "../Threads/Async.h"
#include "../Utils/Common.h"

#include <functional>
#include <string_view>

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

    // A whole command line, used in place of executable and arguments when
    // non-empty. On Windows it is handed to CreateProcessW unchanged (no
    // application name, no quoting), the way JUCE's ChildProcess::start(String)
    // does, so a line such as `cmd /c "vcvars.bat && cmake --build ."` reaches
    // cmd exactly as written. POSIX has no such call and no shell is involved:
    // the line is split by tokeniseCommandLine() and the first token is looked
    // up on PATH like any bare executable; a line with no tokens does not
    // launch.
    std::string commandLine {};

    // When false the child inherits the launcher's stdio rather than having it
    // captured into output()/errorOutput(). Suits long-running children whose
    // output would otherwise buffer unbounded.
    bool captureOutput = true;

    // When true the child's stderr goes into the same pipe as its stdout, so
    // output() holds both streams interleaved in the order the child wrote
    // them and errorOutput() stays empty. Only applies with captureOutput.
    bool mergeErrorOutput = false;

    // Called with each chunk of stdout (stdout and stderr when
    // mergeErrorOutput is set) as it arrives, on the background reader thread,
    // just before the chunk is appended to output(). Chunks follow the pipe's
    // reads, not lines, and may split a UTF-8 sequence. Only applies with
    // captureOutput.
    std::function<void(std::string_view)> onOutput {};

    // When true the child is launched detached: destroying the Process never
    // kills it, so it survives both the object and the launching process. For
    // hand-off launches, e.g. an updater starting its replacement before
    // exiting. Implies captureOutput = false; the launcher is expected to exit
    // soon after (a detached child that outlives a still-running launcher is
    // not reaped on POSIX until the launcher exits).
    bool detached = false;

    // Windows only: the child is created with CREATE_NO_WINDOW, so a
    // console-subsystem child started from a GUI process does not pop a console
    // window of its own. Ignored for detached launches, which already come
    // without a console.
    bool noWindow = false;

    // When true the child starts in a process group of its own, which
    // Process::requestStop() then signals as a whole, children of its own
    // included. POSIX makes the child the leader of a new group (its group id
    // is its pid). Windows passes CREATE_NEW_PROCESS_GROUP, which also starts
    // the child with Ctrl+C disabled, as an ignored SIGINT is inherited on
    // POSIX: Ctrl+C pressed in the shared console no longer reaches it, and a
    // child that wants it back calls SetConsoleCtrlHandler(nullptr, FALSE).
    // Ctrl+Break, and requestStop(), still do. On POSIX the new group is a
    // background group of the launcher's terminal: Ctrl+C there no longer
    // reaches the child either, and a child reading the terminal it inherited
    // (captureOutput = false) is stopped by SIGTTIN, or by SIGTTOU writing to
    // it under `stty tostop`. terminate() and kill() still reach the child
    // alone, not the rest of its group. Ignored for detached launches.
    bool newProcessGroup = false;

    // When true, terminate() and kill() end the processes the child started as
    // well as the child, and so does destroying the Process before wait() has
    // returned, even when the child itself has already exited, so none of them
    // is left holding the captured output open. Windows puts the child in a
    // job object of its own before it runs, and ends the job; every process it
    // starts joins the job unless it breaks away. POSIX starts the child in a
    // process group of its own, as newProcessGroup does, and signals the group;
    // a process that moved to another group (ninja runs each job in one) is not
    // reached. The child then leaves the terminal's foreground group: Ctrl+C
    // pressed for this process no longer reaches it or what it started, and if
    // this process dies without destroying the Process they run to completion
    // on their own. Until it is waited for or destroyed, a POSIX child is not
    // reaped by isRunning() or requestStop(), only observed, so its group's id
    // cannot be reused while it may still be signalled. Ignored for detached
    // launches.
    bool killDescendants = false;
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

    // Blocks until the child exits and its output has been read, and returns
    // its exit code. Call it from one thread at a time: isRunning() and
    // requestStop() may be called from others meanwhile, but two concurrent
    // waits on a child with captured output are not supported.
    int wait();
    void terminate();
    void kill();

    // Asks the child to stop, leaving it the chance to shut down cleanly, and
    // returns whether the request was sent. The child may take its time, or
    // ignore it; terminate() and kill() remain the way to end it regardless.
    // POSIX sends SIGTERM, to the whole process group when the child was
    // started with newProcessGroup. Windows has no signal one process can send
    // another, so it raises CTRL_BREAK_EVENT in the child's process group
    // (GenerateConsoleCtrlEvent): the child sees SIGBREAK, or its console
    // control handler runs, and one with neither ends with exit code
    // STATUS_CONTROL_C_EXIT. The event travels through a console, so on
    // Windows this returns false without sending anything unless the child was
    // started with newProcessGroup and shares this process's console: not with
    // noWindow (the child gets a console of its own), not detached (it gets
    // none), and not when this process has no console (a GUI app; its
    // console-subsystem children get consoles of their own). Returns false
    // once the child has exited, even if others of its group still run. Safe
    // to call while another thread is in wait().
    bool requestStop();

    [[nodiscard]] std::string output() const;
    [[nodiscard]] std::string errorOutput() const;

private:
    struct Native;
    Pimpl<Native> impl;
};

// Splits a command line the way JUCE's StringArray::fromTokens(line, true)
// does: breaks at each space, tab, CR or LF outside double quotes, keeps the
// quote characters inside the tokens, and drops empty tokens. The first token
// alone is then unquoted (one leading and one trailing ' or " removed). This
// is what ProcessOptions::commandLine runs on POSIX.
Vector<std::string> tokeniseCommandLine(std::string_view line);

// Convenience: launch, block until the child exits, and return everything it
// produced. The child's stdin is closed first, so a child that reads it sees
// end-of-file rather than waiting forever. Runs on the calling thread.
ProcessResult run(ProcessOptions options);
ProcessResult run(const std::string& executable,
                  const Vector<std::string>& arguments = {});

// Same as run(), but on a background thread; the returned Async resolves on the
// main thread once the child exits.
Threads::Async<ProcessResult> runAsync(ProcessOptions options);
} // namespace eacp::Processes
