#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#include <mutex>
#endif

namespace term
{
struct PtySize
{
    int cols = 80;
    int rows = 24;
};

struct PtyOptions
{
    PtySize size;

    // Where the shell starts; empty means the user's home directory.
    std::string workingDirectory;

    // Run this command (through the shell, so the login environment and
    // PATH apply) instead of an interactive shell; the PTY ends when it
    // exits. Empty runs the user's shell.
    std::string command;
};

// A pseudo-terminal running the user's login shell. Output is delivered on a
// background reader thread via onOutput — marshal to the UI thread before
// touching any view state. onExit fires (also on the reader thread) when the
// shell terminates.
class Pty
{
public:
    Pty() = default;
    ~Pty();

    Pty(const Pty&) = delete;
    Pty& operator=(const Pty&) = delete;

    bool start(const PtyOptions& options,
               std::function<void(std::string)> onOutput,
               std::function<void()> onExit);

    void write(std::string_view data);
    void resize(const PtySize& size);
    bool isRunning() const;
    void shutdown();

    // Name of the process group currently owning the terminal (what the user
    // sees running: zsh, claude, nvim, ...). Empty when unknown.
    std::string foregroundProcess() const;

    // The shell's live working directory, read from the kernel — no shell
    // integration needed. Empty when unknown.
    std::string currentWorkingDirectory() const;

private:
#if defined(_WIN32)
    // ConPTY state. The pseudoconsole handle is guarded by consoleLock: the
    // waiter thread closes it when the shell exits, which must not race
    // resize() on the UI thread.
    void closeConsole();

    void* console = nullptr;
    void* process = nullptr;
    void* inputWrite = nullptr;
    void* outputRead = nullptr;
    std::thread reader;
    std::thread waiter;
    mutable std::mutex consoleLock;
#else
    int fd = -1;
    long pid = -1;
    std::thread reader;
#endif
};
} // namespace term
