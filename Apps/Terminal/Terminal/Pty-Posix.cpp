#include "Pty.h"

#include <csignal>
#include <cstdlib>
#include <string>

#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

namespace term
{
namespace
{
[[noreturn]] void execShell()
{
    for (auto sig: {SIGINT, SIGQUIT, SIGTSTP, SIGPIPE, SIGCHLD})
        signal(sig, SIG_DFL);

    setenv("TERM", "xterm-256color", 1);
    setenv("COLORTERM", "truecolor", 1);
    unsetenv("TERM_PROGRAM");

    if (getenv("LANG") == nullptr)
        setenv("LANG", "en_US.UTF-8", 1);

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

bool Pty::start(const PtySize& size,
                std::function<void(std::string)> onOutput,
                std::function<void()> onExit)
{
    if (fd >= 0)
        return false;

    auto ws = toWinsize(size);
    auto masterFd = -1;
    const auto child = forkpty(&masterFd, nullptr, nullptr, &ws);

    if (child < 0)
        return false;

    if (child == 0)
        execShell();

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
