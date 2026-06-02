#include "Process.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

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

[[noreturn]] void runChild(int inRead,
                           int outWrite,
                           int errWrite,
                           const ProcessOptions& options,
                           std::vector<char*>& argv)
{
    dup2(inRead, STDIN_FILENO);
    dup2(outWrite, STDOUT_FILENO);
    dup2(errWrite, STDERR_FILENO);

    if (!options.workingDirectory.empty())
        if (chdir(options.workingDirectory.c_str()) != 0)
            _exit(127);

    for (const auto& var: options.environment)
        setenv(var.name.c_str(), var.value.c_str(), 1);

    execvp(argv[0], argv.data());
    _exit(127);
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

    bool launched() const { return pid > 0; }
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
    void launch(const ProcessOptions& options)
    {
        int inPipe[2];
        int outPipe[2];
        int errPipe[2];

        if (pipe(inPipe) != 0)
            return;

        if (pipe(outPipe) != 0)
        {
            ::close(inPipe[0]);
            ::close(inPipe[1]);
            return;
        }

        if (pipe(errPipe) != 0)
        {
            ::close(inPipe[0]);
            ::close(inPipe[1]);
            ::close(outPipe[0]);
            ::close(outPipe[1]);
            return;
        }

        auto argStrings = std::vector<std::string> {options.executable};
        for (const auto& arg: options.arguments)
            argStrings.push_back(arg);

        auto argv = std::vector<char*> {};
        for (auto& s: argStrings)
            argv.push_back(s.data());
        argv.push_back(nullptr);

        pid = fork();

        if (pid < 0)
        {
            pid = -1;
            ::close(inPipe[0]);
            ::close(inPipe[1]);
            ::close(outPipe[0]);
            ::close(outPipe[1]);
            ::close(errPipe[0]);
            ::close(errPipe[1]);
            return;
        }

        if (pid == 0)
        {
            ::close(inPipe[1]);
            ::close(outPipe[0]);
            ::close(errPipe[0]);
            runChild(inPipe[0], outPipe[1], errPipe[1], options, argv);
        }

        ::close(inPipe[0]);
        ::close(outPipe[1]);
        ::close(errPipe[1]);
        inputFd = inPipe[1];

        outReader = std::thread([this, fd = outPipe[0]] { drain(fd, outBuffer); });
        errReader = std::thread([this, fd = errPipe[0]] { drain(fd, errBuffer); });
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
