#pragma once

#include "Pty.h"

#include <memory>
#include <string>

namespace term
{
// What a pane needs from the process behind it, regardless of where that
// process lives: in this app (LocalShell wraps a Pty) or held by the
// session daemon so it survives the GUI (RemoteShell in DaemonClient.cpp).
//
// Output may arrive on any thread (local: the PTY reader thread; remote:
// the main thread) — receivers marshal for themselves. terminate() ends the
// process on purpose (a closed pane); detach() just stops listening, which
// for a daemon shell means it keeps running.
class Shell
{
public:
    virtual ~Shell() = default;

    virtual bool start(const PtyOptions& options,
                       std::function<void(std::string)> onOutput,
                       std::function<void()> onExit) = 0;

    virtual void write(std::string_view data) = 0;
    virtual void resize(const PtySize& size) = 0;

    virtual std::string foregroundProcess() const = 0;
    virtual std::string currentWorkingDirectory() const = 0;

    virtual void terminate() = 0;
    virtual void detach() = 0;
};

// In-process shell: the pane owns the PTY directly. detach() still kills —
// a local shell cannot outlive its owner.
class LocalShell final : public Shell
{
public:
    bool start(const PtyOptions& options,
               std::function<void(std::string)> onOutput,
               std::function<void()> onExit) override
    {
        return pty.start(options, std::move(onOutput), std::move(onExit));
    }

    void write(std::string_view data) override { pty.write(data); }
    void resize(const PtySize& size) override { pty.resize(size); }

    std::string foregroundProcess() const override
    {
        return pty.foregroundProcess();
    }

    std::string currentWorkingDirectory() const override
    {
        return pty.currentWorkingDirectory();
    }

    void terminate() override { pty.shutdown(); }
    void detach() override { pty.shutdown(); }

private:
    Pty pty;
};

// A daemon-backed shell when the daemon answers, a local one otherwise.
// Defined in DaemonClient.cpp.
std::unique_ptr<Shell> makeShell(const std::string& shellId);
} // namespace term
