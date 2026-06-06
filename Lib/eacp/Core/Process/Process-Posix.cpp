#include "Process.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace eacp::Processes
{
namespace
{
void ignoreSigPipeOnce()
{
    static std::once_flag flag;
    std::call_once(flag, [] { std::signal(SIGPIPE, SIG_IGN); });
}

// Copies the parent environment and applies overrides (replaced by name, else
// appended). Built in the parent: posix_spawn execs without running arbitrary
// code in the child, so there is no fork/setenv-in-child deadlock hazard.
Vector<std::string> buildEnvironment(const Vector<EnvironmentVariable>& overrides)
{
    auto entries = Vector<std::string> {};

    for (auto entry = environ; *entry != nullptr; ++entry)
        entries.push_back(*entry);

    for (const auto& var: overrides)
    {
        auto prefix = var.name + "=";
        auto replaced = false;

        for (auto& entry: entries)
            if (entry.rfind(prefix, 0) == 0)
            {
                entry = prefix + var.value;
                replaced = true;
                break;
            }

        if (!replaced)
            entries.push_back(prefix + var.value);
    }

    return entries;
}

// Null-terminated char* view over `strings` for the argv/envp the spawn syscall
// requires. The backing strings must outlive the returned pointers.
Vector<char*> toCArray(Vector<std::string>& strings)
{
    auto pointers = Vector<char*> {};

    for (auto& s: strings)
        pointers.push_back(s.data());

    pointers.push_back(nullptr);
    return pointers;
}
} // namespace

struct Process::Native
{
    explicit Native(ProcessOptions options)
    {
        ignoreSigPipeOnce();
        launch(options);
    }

    ~Native()
    {
        if (isRunning())
            ::kill(pid, SIGKILL);

        closeInput();
        joinReaders();
        reap(true);
    }

    bool launched() const { return pid > 0 || execFailed; }
    long id() const { return (long) pid; }

    bool isRunning() const
    {
        if (pid <= 0)
            return false;

        return !reap(false);
    }

    int wait()
    {
        joinReaders();
        reap(true);
        return exitCode();
    }

    void terminate()
    {
        if (pid > 0)
            ::kill(pid, SIGTERM);
    }

    void kill()
    {
        if (pid > 0)
            ::kill(pid, SIGKILL);
    }

    bool writeToInput(const std::string& data)
    {
        if (inputFd < 0)
            return false;

        auto total = std::size_t {0};

        while (total < data.size())
        {
            auto written =
                ::write(inputFd, data.data() + total, data.size() - total);

            if (written < 0)
            {
                if (errno == EINTR)
                    continue;

                return false;
            }

            total += (std::size_t) written;
        }

        return true;
    }

    void closeInput()
    {
        if (inputFd >= 0)
        {
            ::close(inputFd);
            inputFd = -1;
        }
    }

    std::string output() const
    {
        auto lock = std::lock_guard {bufferMutex};
        return outBuffer;
    }

    std::string errorOutput() const
    {
        auto lock = std::lock_guard {bufferMutex};
        return errBuffer;
    }

private:
    // Spawns the child with posix_spawn: the env/argv are built in the parent
    // and stdio is wired through file actions, so no arbitrary code (and no
    // allocation) runs in the child — avoiding the fork-in-a-threaded-process
    // deadlock. posix_spawnp keeps PATH lookup for bare executables.
    void launch(const ProcessOptions& options)
    {
        auto argStrings = Vector<std::string> {options.executable};
        for (const auto& arg: options.arguments)
            argStrings.push_back(arg);

        auto envStrings = buildEnvironment(options.environment);

        auto argv = toCArray(argStrings);
        auto envp = toCArray(envStrings);

        int inPipe[2] = {-1, -1};
        int outPipe[2] = {-1, -1};
        int errPipe[2] = {-1, -1};

        if (options.captureOutput && !createPipes(inPipe, outPipe, errPipe))
            return;

        auto actions = posix_spawn_file_actions_t {};
        posix_spawn_file_actions_init(&actions);

        if (options.captureOutput)
        {
            posix_spawn_file_actions_adddup2(&actions, inPipe[0], STDIN_FILENO);
            posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
            posix_spawn_file_actions_adddup2(&actions, errPipe[1], STDERR_FILENO);

            for (auto fd: {inPipe[0],
                           inPipe[1],
                           outPipe[0],
                           outPipe[1],
                           errPipe[0],
                           errPipe[1]})
                posix_spawn_file_actions_addclose(&actions, fd);
        }

        if (!options.workingDirectory.empty())
        {
            // _np is the only variant available at our deployment target; the
            // non-suffixed addchdir is macOS 26+ only.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            posix_spawn_file_actions_addchdir_np(&actions,
                                                 options.workingDirectory.c_str());
#pragma clang diagnostic pop
        }

        auto result = posix_spawnp(&pid,
                                   options.executable.c_str(),
                                   &actions,
                                   nullptr,
                                   argv.data(),
                                   envp.data());

        posix_spawn_file_actions_destroy(&actions);

        if (result != 0)
        {
            // posix_spawn reports a missing/non-executable file as a launch error
            // (no child), where fork+execvp produced a child that exited 127.
            // Preserve that 127 "command not found" contract for callers.
            pid = -1;
            execFailed = true;
            closeAllPipes(inPipe, outPipe, errPipe);
            return;
        }

        if (options.captureOutput)
            adoptPipes(inPipe, outPipe, errPipe);
    }

    static bool createPipes(int (&in)[2], int (&out)[2], int (&err)[2])
    {
        if (pipe(in) != 0)
            return false;

        if (pipe(out) != 0)
        {
            ::close(in[0]);
            ::close(in[1]);
            return false;
        }

        if (pipe(err) != 0)
        {
            ::close(in[0]);
            ::close(in[1]);
            ::close(out[0]);
            ::close(out[1]);
            return false;
        }

        return true;
    }

    static void closeAllPipes(int (&in)[2], int (&out)[2], int (&err)[2])
    {
        for (auto fd: {in[0], in[1], out[0], out[1], err[0], err[1]})
            if (fd >= 0)
                ::close(fd);
    }

    // Keep the parent ends and stream the child's output; the child's ends were
    // dup'd onto its stdio by the file actions.
    void adoptPipes(int (&in)[2], int (&out)[2], int (&err)[2])
    {
        ::close(in[0]);
        ::close(out[1]);
        ::close(err[1]);
        inputFd = in[1];

        outReader = std::thread([this, fd = out[0]] { drain(fd, outBuffer); });
        errReader = std::thread([this, fd = err[0]] { drain(fd, errBuffer); });
    }

    void drain(int fd, std::string& dest)
    {
        char buffer[4096];

        for (;;)
        {
            auto count = ::read(fd, buffer, sizeof buffer);

            if (count > 0)
            {
                auto lock = std::lock_guard {bufferMutex};
                dest.append(buffer, (std::size_t) count);
            }
            else if (count < 0 && errno == EINTR)
            {
                continue;
            }
            else
            {
                break;
            }
        }

        ::close(fd);
    }

    void joinReaders()
    {
        if (outReader.joinable())
            outReader.join();

        if (errReader.joinable())
            errReader.join();
    }

    bool reap(bool blocking) const
    {
        auto lock = std::lock_guard {reapMutex};

        if (reaped)
            return true;

        if (pid <= 0)
            return false;

        int status = 0;
        auto result = waitpid(pid, &status, blocking ? 0 : WNOHANG);

        if (result == 0)
            return false;

        if (result > 0)
            exitStatus = status;

        reaped = true;
        return true;
    }

    int exitCode() const
    {
        if (execFailed)
            return 127;

        auto lock = std::lock_guard {reapMutex};

        if (!reaped)
            return -1;

        if (WIFEXITED(exitStatus))
            return WEXITSTATUS(exitStatus);

        if (WIFSIGNALED(exitStatus))
            return 128 + WTERMSIG(exitStatus);

        return -1;
    }

    pid_t pid = -1;
    bool execFailed = false; // posix_spawn couldn't exec → report 127
    int inputFd = -1;

    std::thread outReader;
    std::thread errReader;

    mutable std::mutex bufferMutex;
    std::string outBuffer;
    std::string errBuffer;

    mutable std::mutex reapMutex;
    mutable bool reaped = false;
    mutable int exitStatus = 0;
};

Process::Process(ProcessOptions options)
    : impl(std::move(options))
{
}

Process::~Process() = default;

bool Process::launched() const
{
    return impl->launched();
}
bool Process::isRunning() const
{
    return impl->isRunning();
}
long Process::id() const
{
    return impl->id();
}

bool Process::writeToInput(const std::string& data)
{
    return impl->writeToInput(data);
}

void Process::closeInput()
{
    impl->closeInput();
}

int Process::wait()
{
    return impl->wait();
}
void Process::terminate()
{
    impl->terminate();
}
void Process::kill()
{
    impl->kill();
}

std::string Process::output() const
{
    return impl->output();
}
std::string Process::errorOutput() const
{
    return impl->errorOutput();
}
} // namespace eacp::Processes
