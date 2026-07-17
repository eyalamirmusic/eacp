#pragma once

#include "Pty.h"

#include <eacp/Core/Threads/Timer.h>
#include <eacp/Network/IPC/Messenger.h>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace term
{
// The app's end of the session daemon: one Messenger conversation carrying
// every pane. Shells are keyed by stable id — spawn adopts a still-running
// shell (replaying its recent output) or starts a fresh one; release stops
// listening without killing, which is how shells outlive the GUI.
//
// Main-thread object, like the Messenger it rides on.
class DaemonClient
{
public:
    struct Routes
    {
        std::function<void(std::string)> onOutput = [](const std::string&) {};
        eacp::Callback onExit = [] {};
    };

    struct ShellInfo
    {
        std::string foregroundProcess;
        std::string workingDirectory;
    };

    // Dials the daemon, launching the bundled TerminalDaemon binary when
    // nobody answers, and calls whenReady once the outcome is known either
    // way. get() answers null until then — and permanently when the daemon
    // is unreachable, which drops every pane back to in-process shells.
    static void initialize(const eacp::Callback& whenReady);
    static DaemonClient* get();
    static void teardown();

    bool isConnected() const;

    void spawn(const std::string& id,
               const PtySize& size,
               const std::string& cwd,
               Routes routes);
    void write(const std::string& id, std::string_view data);
    void resize(const std::string& id, const PtySize& size);
    void kill(const std::string& id);
    void release(const std::string& id);

    ShellInfo infoFor(const std::string& id) const;

    // Tears the whole server down — every surviving shell dies. The tray's
    // "kill everything" path.
    void killServer();

private:
    explicit DaemonClient(std::unique_ptr<eacp::IPC::Messenger> messengerToUse);

    void handleMessage(const std::string& body);
    void requestInfo();

    std::unique_ptr<eacp::IPC::Messenger> messenger;
    std::unordered_map<std::string, Routes> routes;
    std::unordered_map<std::string, ShellInfo> info;
    std::unique_ptr<eacp::Threads::Timer> infoTimer;
};
} // namespace term
