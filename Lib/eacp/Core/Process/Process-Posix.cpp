#include "Process.h"
#include "SpawnDirectory-Posix.h"
#include "../Utils/Environment-Posix.h"
#include "../Utils/Logging.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <mutex>
#include <thread>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

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
    auto entries = copyEnvironmentEntries();

    for (const auto& var: overrides)
    {
        auto prefix = var.name + "=";
        auto replaced = false;

        for (auto& entry: entries)
            if (entry.starts_with(prefix))
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

bool startsInGroupOfItsOwn(const ProcessOptions& options)
{
    return (options.newProcessGroup || options.killDescendants) && !options.detached;
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
        : detached(options.detached)
        , leadsItsOwnGroup(startsInGroupOfItsOwn(options))
        , killsDescendants(options.killDescendants && !options.detached)
        , onOutput(std::move(options.onOutput))
    {
        ignoreSigPipeOnce();

        if (detached)
            options.captureOutput = false;

        launch(options);
    }

    ~Native()
    {
        // A detached child is deliberately left running and unreaped — it
        // outlives this object (and typically this process).
        if (detached)
            return;

        if (isStillOwedAnEnd())
            kill();

        closeInput();
        joinReaders();
        reap(true);
    }

    // A child waited for is done with. One that was not is ended, and with
    // killsDescendants so is what it started, even if the child itself has
    // exited: they may still hold the captured output open.
    bool isStillOwedAnEnd() const
    {
        return killsDescendants ? !waitedFor : isRunning();
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
        waitedFor = true;
        return exitCode();
    }

    void terminate() { end(SIGTERM); }

    void kill() { end(SIGKILL); }

    // Sent under the lock reaping takes, so the child cannot be reaped, and its
    // pid reused, between the check and the signal.
    bool requestStop()
    {
        auto lock = std::lock_guard {reapMutex};

        if (pid <= 0 || hasExited())
            return false;

        return ::kill(leadsItsOwnGroup ? -pid : pid, SIGTERM) == 0;
    }

    // The group is signalled under the lock reaping takes, so its leader cannot
    // be reaped, and the group id reused, in between. A leader that has exited
    // but is not yet reaped still holds the id, so whatever it left running in
    // the group is reached.
    void end(int signal)
    {
        if (!killsDescendants)
        {
            if (pid > 0)
                ::kill(pid, signal);

            return;
        }

        auto lock = std::lock_guard {reapMutex};

        if (pid > 0 && !reaped)
            ::kill(-pid, signal);
    }

    bool writeToInput(const std::string& data)
    {
        if (inputFd < 0)
            return false;

        auto total = 0;
        const auto size = (int) data.size();

        while (total < size)
        {
            auto written =
                ::write(inputFd, data.data() + total, (std::size_t) (size - total));

            if (written < 0)
            {
                if (errno == EINTR)
                    continue;

                return false;
            }

            total += (int) written;
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
    static Vector<std::string> argumentsFor(const ProcessOptions& options)
    {
        if (!options.commandLine.empty())
            return tokeniseCommandLine(options.commandLine);

        auto argStrings = Vector<std::string> {options.executable};
        for (const auto& arg: options.arguments)
            argStrings.push_back(arg);

        return argStrings;
    }

    void launch(const ProcessOptions& options)
    {
        auto argStrings = argumentsFor(options);

        if (!options.commandLine.empty()
            && (argStrings.empty() || argStrings.front().empty()))
        {
            LOG("Process: nothing to run in command line '",
                options.commandLine,
                "'");
            return;
        }

        const auto executable = argStrings.front();
        auto envStrings = buildEnvironment(options.environment);

        auto argv = toCArray(argStrings);
        auto envp = toCArray(envStrings);

        int inPipe[2] = {-1, -1};
        int outPipe[2] = {-1, -1};
        int errPipe[2] = {-1, -1};

        const auto merge = options.mergeErrorOutput;

        if (options.captureOutput && !createPipes(inPipe, outPipe, errPipe, !merge))
            return;

        auto actions = posix_spawn_file_actions_t {};
        posix_spawn_file_actions_init(&actions);

        if (options.captureOutput)
        {
            posix_spawn_file_actions_adddup2(&actions, inPipe[0], STDIN_FILENO);
            posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
            posix_spawn_file_actions_adddup2(
                &actions, merge ? outPipe[1] : errPipe[1], STDERR_FILENO);
        }

        if (!options.workingDirectory.empty()
            && !addSpawnWorkingDirectory(actions, options.workingDirectory))
        {
            LOG("Process: cannot start ",
                executable,
                " in ",
                options.workingDirectory);
            posix_spawn_file_actions_destroy(&actions);
            closeAllPipes(inPipe, outPipe, errPipe);
            return;
        }

        auto attr = posix_spawnattr_t {};
        posix_spawnattr_init(&attr);

        setSpawnFlags(attr, actions, options);

        auto result = posix_spawnp(
            &pid, executable.c_str(), &actions, &attr, argv.data(), envp.data());

        posix_spawnattr_destroy(&attr);
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

    static void setSpawnFlags(posix_spawnattr_t& attr,
                              [[maybe_unused]] posix_spawn_file_actions_t& actions,
                              const ProcessOptions& options)
    {
        auto spawnFlags = short {0};

#if defined(__APPLE__)
        // pipe() and fcntl() are two calls, so a sibling spawned between them
        // inherits the pipe; this flag closes everything not dup2'd onto 0/1/2.
        spawnFlags |= POSIX_SPAWN_CLOEXEC_DEFAULT;

        if (!options.captureOutput)
        {
            for (auto fd: {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO})
                posix_spawn_file_actions_addinherit_np(&actions, fd);
        }
#endif

#if defined(POSIX_SPAWN_SETSID)
        // A detached child becomes its own session leader so it survives the
        // launcher's terminal/session, not just the launcher process.
        if (options.detached)
            spawnFlags |= POSIX_SPAWN_SETSID;
#endif

        // newProcessGroup and killDescendants are ignored for a detached
        // child: where setsid is available it already leads a group, and a
        // session leader cannot change its group.
        if (startsInGroupOfItsOwn(options))
        {
            spawnFlags |= POSIX_SPAWN_SETPGROUP;
            posix_spawnattr_setpgroup(&attr, 0);
        }

        posix_spawnattr_setflags(&attr, spawnFlags);
    }

    // Close-on-exec, so a child started concurrently by another Process never
    // inherits these ends; dup2 onto 0/1/2 clears the flag on the copy.
    static bool openCloseOnExecPipe(int (&fds)[2])
    {
#if defined(__APPLE__)
        if (pipe(fds) != 0)
            return false;

        fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        fcntl(fds[1], F_SETFD, FD_CLOEXEC);
        return true;
#else
        return pipe2(fds, O_CLOEXEC) == 0;
#endif
    }

    // Without a separate error pipe (merged stderr) err is left at {-1, -1}.
    static bool
        createPipes(int (&in)[2], int (&out)[2], int (&err)[2], bool separateErr)
    {
        if (!openCloseOnExecPipe(in))
            return false;

        if (!openCloseOnExecPipe(out))
        {
            ::close(in[0]);
            ::close(in[1]);
            return false;
        }

        if (separateErr && !openCloseOnExecPipe(err))
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
        inputFd = in[1];

        outReader =
            std::thread([this, fd = out[0]] { drain(fd, outBuffer, onOutput); });

        if (err[0] >= 0)
        {
            ::close(err[1]);
            errReader =
                std::thread([this, fd = err[0]] { drain(fd, errBuffer, nullptr); });
        }
    }

    void drain(int fd,
               std::string& dest,
               const std::function<void(std::string_view)>& onChunk)
    {
        char buffer[4096];

        for (;;)
        {
            auto count = ::read(fd, buffer, sizeof buffer);

            if (count > 0)
            {
                if (onChunk)
                    onChunk(std::string_view {buffer, (std::size_t) count});

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

    // The blocking wait leaves the child unreaped (WNOWAIT) and holds no lock,
    // so isRunning(), requestStop() and kill() on other threads still answer
    // while one thread waits; the status is then collected once, under the
    // lock, without blocking. A child already reaped is never waited on: its
    // pid may now be another child's. waitid's outcome is only a hint: should
    // it fail, the status is polled instead, still never blocking under the
    // lock.
    bool reap(bool blocking) const
    {
        if (pid <= 0)
            return false;

        if (!blocking)
        {
            auto lock = std::lock_guard {reapMutex};
            return hasExited();
        }

        for (;;)
        {
            {
                auto lock = std::lock_guard {reapMutex};

                if (reapIfExited(WNOHANG))
                    return true;
            }

            if (!waitUntilExited())
                std::this_thread::sleep_for(std::chrono::milliseconds {10});
        }
    }

    bool waitUntilExited() const
    {
        auto info = siginfo_t {};
        auto result = waitid(P_PID, (id_t) pid, &info, WEXITED | WNOWAIT);

        while (result != 0 && errno == EINTR)
            result = waitid(P_PID, (id_t) pid, &info, WEXITED | WNOWAIT);

        return result == 0;
    }

    // Needs reapMutex held. A child whose descendants are to be ended is not
    // reaped here, only observed: until wait() or the destructor has done with
    // its group, the unreaped leader keeps the group's id from being reused.
    bool hasExited() const
    {
        if (!killsDescendants || reaped)
            return reapIfExited(WNOHANG);

        auto info = siginfo_t {};
        auto options = WEXITED | WNOHANG | WNOWAIT;
        auto result = waitid(P_PID, (id_t) pid, &info, options);

        while (result != 0 && errno == EINTR)
            result = waitid(P_PID, (id_t) pid, &info, options);

        if (result != 0)
            return reapIfExited(WNOHANG);

        return info.si_pid != 0;
    }

    // Needs reapMutex held.
    bool reapIfExited(int waitOptions) const
    {
        if (reaped)
            return true;

        if (pid <= 0)
            return false;

        int status = 0;
        auto result = waitpid(pid, &status, waitOptions);

        while (result < 0 && errno == EINTR)
            result = waitpid(pid, &status, waitOptions);

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
    const bool detached;
    const bool leadsItsOwnGroup;
    const bool killsDescendants;
    const std::function<void(std::string_view)> onOutput;
    int inputFd = -1;

    std::thread outReader;
    std::thread errReader;

    mutable std::mutex bufferMutex;
    std::string outBuffer;
    std::string errBuffer;

    mutable std::mutex reapMutex;
    mutable bool reaped = false;
    mutable int exitStatus = 0;
    bool waitedFor = false;
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
bool Process::requestStop()
{
    return impl->requestStop();
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
