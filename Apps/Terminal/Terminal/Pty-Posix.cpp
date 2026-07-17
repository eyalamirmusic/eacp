#include "Pty.h"

#include <csignal>
#include <cstdlib>
#include <string>

#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libproc.h>
#include <util.h>
#else
#include <pty.h>
#endif

namespace term
{
namespace
{
[[noreturn]] void execShell(const std::string& workingDirectory)
{
    for (auto sig: {SIGINT, SIGQUIT, SIGTSTP, SIGPIPE, SIGCHLD})
        signal(sig, SIG_DFL);

    setenv("TERM", "xterm-256color", 1);
    setenv("COLORTERM", "truecolor", 1);
    unsetenv("TERM_PROGRAM");

    if (getenv("LANG") == nullptr)
        setenv("LANG", "en_US.UTF-8", 1);

    if (workingDirectory.empty() || chdir(workingDirectory.c_str()) != 0)
        if (const auto* home = getenv("HOME"))
            chdir(home);

    const auto* shell = getenv("SHELL");

    if (shell == nullptr || shell[0] == '\0')
        shell = "/bin/zsh";

    // A leading dash asks the shell to start as a login shell, so the user's
    // usual profile and prompt come up.
    auto shellPath = std::string {shell};
    const auto slash = shellPath.find_last_of('/');
    const auto loginName =
        "-" + (slash == std::string::npos ? shellPath : shellPath.substr(slash + 1));

    execl(shell, loginName.c_str(), (char*) nullptr);
    _exit(127);
}

winsize toWinsize(const PtySize& size)
{
    auto ws = winsize {};
    ws.ws_col = (unsigned short) size.cols;
    ws.ws_row = (unsigned short) size.rows;
    return ws;
}
} // namespace

Pty::~Pty()
{
    shutdown();
}

bool Pty::start(const PtyOptions& options,
                std::function<void(std::string)> onOutput,
                std::function<void()> onExit)
{
    if (fd >= 0)
        return false;

    auto ws = toWinsize(options.size);
    auto masterFd = -1;
    const auto child = forkpty(&masterFd, nullptr, nullptr, &ws);

    if (child < 0)
        return false;

    if (child == 0)
        execShell(options.workingDirectory);

    fd = masterFd;
    pid = child;

    reader = std::thread(
        [masterFd, output = std::move(onOutput), exit = std::move(onExit)]
        {
            char buffer[65536];

            while (true)
            {
                const auto count = read(masterFd, buffer, sizeof(buffer));

                if (count > 0)
                {
                    output(std::string {buffer, (std::size_t) count});
                    continue;
                }

                if (count < 0 && (errno == EINTR || errno == EAGAIN))
                    continue;

                break;
            }

            exit();
        });

    return true;
}

void Pty::write(std::string_view data)
{
    auto remaining = data;

    while (fd >= 0 && !remaining.empty())
    {
        const auto written = ::write(fd, remaining.data(), remaining.size());

        if (written > 0)
        {
            remaining.remove_prefix((std::size_t) written);
            continue;
        }

        if (written < 0 && (errno == EINTR || errno == EAGAIN))
            continue;

        break;
    }
}

void Pty::resize(const PtySize& size)
{
    if (fd < 0)
        return;

    auto ws = toWinsize(size);
    ioctl(fd, TIOCSWINSZ, &ws);
}

std::string Pty::foregroundProcess() const
{
    if (fd < 0)
        return {};

    const auto pgid = tcgetpgrp(fd);

    if (pgid <= 0)
        return {};

#if defined(__APPLE__)
    char name[2 * MAXCOMLEN] = {};

    if (proc_name(pgid, name, sizeof(name)) > 0)
        return name;

    return {};
#else
    auto comm = std::string {"/proc/"} + std::to_string(pgid) + "/comm";

    if (auto* file = fopen(comm.c_str(), "r"))
    {
        char name[256] = {};
        const auto* result = fgets(name, sizeof(name), file);
        fclose(file);

        if (result != nullptr)
        {
            auto text = std::string {name};

            if (!text.empty() && text.back() == '\n')
                text.pop_back();

            return text;
        }
    }

    return {};
#endif
}

std::string Pty::currentWorkingDirectory() const
{
    if (pid <= 0)
        return {};

#if defined(__APPLE__)
    auto info = proc_vnodepathinfo {};

    if (proc_pidinfo((pid_t) pid, PROC_PIDVNODEPATHINFO, 0, &info, sizeof(info)) > 0)
        return info.pvi_cdir.vip_path;

    return {};
#else
    char path[4096] = {};
    const auto link = "/proc/" + std::to_string(pid) + "/cwd";
    const auto count = readlink(link.c_str(), path, sizeof(path) - 1);
    return count > 0 ? std::string {path, (std::size_t) count} : std::string {};
#endif
}

bool Pty::isRunning() const
{
    if (pid < 0)
        return false;

    return ::kill((pid_t) pid, 0) == 0;
}

void Pty::shutdown()
{
    if (fd >= 0)
    {
        close(fd);
        fd = -1;
    }

    if (pid > 0)
    {
        ::kill((pid_t) pid, SIGHUP);

        auto status = 0;
        waitpid((pid_t) pid, &status, 0);
        pid = -1;
    }

    if (reader.joinable())
        reader.join();
}
} // namespace term
