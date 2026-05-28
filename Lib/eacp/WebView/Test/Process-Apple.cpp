#include "Process.h"

#include <array>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <spawn.h>
#include <stdexcept>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace eacp::WebView::Test
{

namespace
{

// Shared output state for the two reader threads + the main thread.
// Single mutex + condvar — pollOutput() wakes as soon as ANY stream
// has new bytes, so a chatty stderr can't make a stdout-only caller
// burn its whole timeout, and vice versa.
struct OutputState
{
    mutable std::mutex mutex;
    mutable std::condition_variable cv;
    std::string outBuf;
    std::string errBuf;
    bool outEof = false;
    bool errEof = false;

    void appendOut(std::string_view bytes)
    {
        auto lock = std::lock_guard {mutex};
        outBuf.append(bytes);
        cv.notify_all();
    }

    void appendErr(std::string_view bytes)
    {
        auto lock = std::lock_guard {mutex};
        errBuf.append(bytes);
        cv.notify_all();
    }

    void markOutEof()
    {
        auto lock = std::lock_guard {mutex};
        outEof = true;
        cv.notify_all();
    }

    void markErrEof()
    {
        auto lock = std::lock_guard {mutex};
        errEof = true;
        cv.notify_all();
    }
};

struct ArgvHolder
{
    std::vector<std::string> storage;
    std::vector<char*> argv;
};

ArgvHolder buildArgv(const std::string& executable,
                     const std::vector<std::string>& args)
{
    auto holder = ArgvHolder {};
    holder.storage.reserve(args.size() + 1);
    holder.storage.push_back(executable);
    for (auto& a: args)
        holder.storage.push_back(a);

    holder.argv.reserve(holder.storage.size() + 1);
    for (auto& s: holder.storage)
        holder.argv.push_back(s.data());
    holder.argv.push_back(nullptr);
    return holder;
}

std::map<std::string, std::string> parentEnv()
{
    auto result = std::map<std::string, std::string> {};
    for (auto p = environ; *p != nullptr; ++p)
    {
        auto entry = std::string_view {*p};
        auto eq = entry.find('=');
        if (eq == std::string_view::npos)
            continue;
        result.emplace(std::string(entry.substr(0, eq)),
                       std::string(entry.substr(eq + 1)));
    }
    return result;
}

struct EnvHolder
{
    std::vector<std::string> storage;
    std::vector<char*> envp;
};

EnvHolder buildEnv(const std::map<std::string, std::string>& overrides)
{
    auto merged = parentEnv();
    for (auto& [k, v]: overrides)
    {
        if (v.empty())
            merged.erase(k);
        else
            merged[k] = v;
    }

    auto holder = EnvHolder {};
    holder.storage.reserve(merged.size());
    for (auto& [k, v]: merged)
    {
        auto entry = k;
        entry.reserve(k.size() + 1 + v.size());
        entry.append("=").append(v);
        holder.storage.push_back(std::move(entry));
    }

    holder.envp.reserve(holder.storage.size() + 1);
    for (auto& s: holder.storage)
        holder.envp.push_back(s.data());
    holder.envp.push_back(nullptr);
    return holder;
}

void readerLoop(int fd, OutputState& state, bool isStdout)
{
    char chunk[4096];
    while (true)
    {
        auto n = ::read(fd, chunk, sizeof chunk);
        if (n > 0)
        {
            auto view = std::string_view {chunk, static_cast<std::size_t>(n)};
            if (isStdout)
                state.appendOut(view);
            else
                state.appendErr(view);
            continue;
        }
        if (n == 0)
            break;
        if (errno == EINTR)
            continue;
        break;
    }
    if (isStdout)
        state.markOutEof();
    else
        state.markErrEof();
}

} // namespace

struct Process::Impl
{
    pid_t pid = -1;
    int stdoutFd = -1;
    int stderrFd = -1;
    std::thread stdoutReader;
    std::thread stderrReader;
    OutputState output;
    bool reaped = false;
    int exitCode = 0;
    int signal = 0;

    void startReaders()
    {
        stdoutReader = std::thread {
            [this] { readerLoop(stdoutFd, output, true); }};
        stderrReader = std::thread {
            [this] { readerLoop(stderrFd, output, false); }};
    }

    void joinReaders()
    {
        if (stdoutReader.joinable())
            stdoutReader.join();
        if (stderrReader.joinable())
            stderrReader.join();
    }

    void reapBlocking()
    {
        if (reaped)
            return;
        int status = 0;
        while (true)
        {
            auto r = ::waitpid(pid, &status, 0);
            if (r == pid)
                break;
            if (r < 0 && errno == EINTR)
                continue;
            break;
        }
        if (WIFEXITED(status))
            exitCode = WEXITSTATUS(status);
        if (WIFSIGNALED(status))
            signal = WTERMSIG(status);
        reaped = true;
    }
};

Process::Process(const std::string& executable, const ProcessOptions& options)
    : impl(std::make_unique<Impl>())
{
    auto outPipe = std::array<int, 2> {-1, -1};
    auto errPipe = std::array<int, 2> {-1, -1};

    if (::pipe(outPipe.data()) < 0)
        throw std::runtime_error("Process: stdout pipe() failed: "
                                 + std::string(::strerror(errno)));
    if (::pipe(errPipe.data()) < 0)
    {
        auto saved = errno;
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        throw std::runtime_error("Process: stderr pipe() failed: "
                                 + std::string(::strerror(saved)));
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, outPipe[0]);
    posix_spawn_file_actions_addclose(&actions, errPipe[0]);
    posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, errPipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, outPipe[1]);
    posix_spawn_file_actions_addclose(&actions, errPipe[1]);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                     O_RDONLY, 0);

    if (!options.workingDirectory.empty())
    {
        // posix_spawn_file_actions_addchdir was added in macOS 13;
        // the _np variant is the only option on our 11.0 deployment
        // floor. It is marked deprecated on macOS 26+ but stable in
        // between, so we silence the warning rather than version-
        // gating at runtime for a path test runners rarely take.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        posix_spawn_file_actions_addchdir_np(
            &actions, options.workingDirectory.c_str());
#pragma clang diagnostic pop
    }

    auto argv = buildArgv(executable, options.args);
    auto envp = buildEnv(options.env);

    pid_t pid = -1;
    auto spawnErr = ::posix_spawn(&pid, executable.c_str(), &actions, nullptr,
                                  argv.argv.data(), envp.envp.data());

    posix_spawn_file_actions_destroy(&actions);
    ::close(outPipe[1]);
    ::close(errPipe[1]);

    if (spawnErr != 0)
    {
        ::close(outPipe[0]);
        ::close(errPipe[0]);
        throw std::runtime_error("Process: posix_spawn '" + executable
                                 + "' failed: " + ::strerror(spawnErr));
    }

    impl->pid = pid;
    impl->stdoutFd = outPipe[0];
    impl->stderrFd = errPipe[0];
    impl->startReaders();
}

Process::~Process()
{
    if (impl == nullptr)
        return;

    if (impl->pid > 0 && !impl->reaped)
        terminate();

    impl->joinReaders();

    if (impl->stdoutFd >= 0)
        ::close(impl->stdoutFd);
    if (impl->stderrFd >= 0)
        ::close(impl->stderrFd);
}

std::size_t Process::pollOutput(std::chrono::milliseconds timeout)
{
    auto& s = impl->output;
    auto lock = std::unique_lock {s.mutex};

    auto outBefore = s.outBuf.size();
    auto errBefore = s.errBuf.size();

    s.cv.wait_for(lock, timeout,
                  [&]
                  {
                      return s.outBuf.size() > outBefore
                          || s.errBuf.size() > errBefore
                          || (s.outEof && s.errEof);
                  });

    return (s.outBuf.size() - outBefore) + (s.errBuf.size() - errBefore);
}

std::string Process::stdoutBuffer() const
{
    auto lock = std::lock_guard {impl->output.mutex};
    return impl->output.outBuf;
}

std::string Process::stderrBuffer() const
{
    auto lock = std::lock_guard {impl->output.mutex};
    return impl->output.errBuf;
}

bool Process::hasExited()
{
    if (impl->reaped)
        return true;

    int status = 0;
    auto r = ::waitpid(impl->pid, &status, WNOHANG);
    if (r == 0)
        return false;
    if (r < 0)
        return false;

    if (WIFEXITED(status))
        impl->exitCode = WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        impl->signal = WTERMSIG(status);
    impl->reaped = true;
    return true;
}

void Process::terminate(std::chrono::milliseconds gracePeriod)
{
    if (impl->reaped)
        return;

    ::kill(impl->pid, SIGTERM);

    auto deadline = std::chrono::steady_clock::now() + gracePeriod;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (hasExited())
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds {25});
    }

    ::kill(impl->pid, SIGKILL);
    impl->reapBlocking();
}

int Process::exitCode() const
{
    return impl->exitCode;
}

int Process::terminatingSignal() const
{
    return impl->signal;
}

} // namespace eacp::WebView::Test
