#include "DaemonClient.h"
#include "Protocol.h"
#include "Shell.h"

#include <eacp/Core/Process/Process.h>

#include <filesystem>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace term
{
using namespace eacp;

namespace
{
std::unique_ptr<DaemonClient> instance;

std::string daemonExecutablePath()
{
#if defined(__APPLE__)
    char buffer[4096] = {};
    auto size = (std::uint32_t) sizeof(buffer);

    if (_NSGetExecutablePath(buffer, &size) != 0)
        return {};

    auto path = std::filesystem::path {buffer};
    return (path.parent_path() / "TerminalDaemon").string();
#else
    return {};
#endif
}

void launchDaemon()
{
    const auto executable = daemonExecutablePath();

    if (executable.empty())
        return;

    auto options = Processes::ProcessOptions {};
    options.executable = executable;
    options.detached = true;

    // Detached: destroying the handle leaves the daemon running — the
    // whole point of it.
    auto daemon = Processes::Process {options};
}

std::string toString(int value)
{
    return std::to_string(value);
}
} // namespace

void DaemonClient::initialize(const Callback& whenReady)
{
    auto adopt = [](std::unique_ptr<IPC::Messenger> messenger)
    {
        instance.reset(new DaemonClient {std::move(messenger)});
    };

    // First dial is short: the daemon is either already there or not
    // installed at all. The second, after launching it, allows startup time.
    auto first = std::make_shared<std::unique_ptr<IPC::Messenger>>(
        std::make_unique<IPC::Messenger>(proto::serverName, Time::MS {600}));

    (*first)->onConnected = [first, adopt, whenReady]
    {
        adopt(std::move(*first));
        whenReady();
    };

    (*first)->onDisconnected = [first, adopt, whenReady]
    {
        launchDaemon();

        auto second = std::make_shared<std::unique_ptr<IPC::Messenger>>(
            std::make_unique<IPC::Messenger>(proto::serverName,
                                             Time::MS {4000}));

        (*second)->onConnected = [second, adopt, whenReady]
        {
            adopt(std::move(*second));
            whenReady();
        };

        (*second)->onDisconnected = [second, whenReady]
        {
            second->reset();
            whenReady();
        };

        // Keep the dying first messenger alive until this handler returns.
        Threads::callAsync([first] {});
    };
}

DaemonClient* DaemonClient::get()
{
    return instance.get();
}

void DaemonClient::teardown()
{
    instance.reset();
}

DaemonClient::DaemonClient(std::unique_ptr<IPC::Messenger> messengerToUse)
    : messenger(std::move(messengerToUse))
{
    messenger->onMessage = [this](const std::string& body)
    { handleMessage(body); };

    messenger->onDisconnected = [this]
    {
        // The daemon vanished mid-flight; every remote pane's shell is
        // gone from our point of view.
        auto pending = std::move(routes);
        routes.clear();

        for (auto& [id, route]: pending)
            route.onExit();
    };

    infoTimer = std::make_unique<Threads::Timer>([this] { requestInfo(); }, 1);
}

bool DaemonClient::isConnected() const
{
    return messenger != nullptr && messenger->isConnected();
}

void DaemonClient::handleMessage(const std::string& body)
{
    auto message = proto::parse(body);

    if (message.verb == "output" && !message.args.empty())
    {
        if (auto found = routes.find(message.args[0]); found != routes.end())
            found->second.onOutput(std::move(message.payload));

        return;
    }

    if (message.verb == "exit" && !message.args.empty())
    {
        if (auto found = routes.find(message.args[0]); found != routes.end())
        {
            auto route = std::move(found->second);
            routes.erase(found);
            route.onExit();
        }

        return;
    }

    if (message.verb == "info")
    {
        info.clear();
        auto start = std::size_t {0};

        while (start < message.payload.size())
        {
            const auto end =
                std::min(message.payload.find('\n', start),
                         message.payload.size());
            const auto line = message.payload.substr(start, end - start);
            start = end + 1;

            const auto firstTab = line.find('\t');
            const auto secondTab = line.find('\t', firstTab + 1);

            if (firstTab == std::string::npos
                || secondTab == std::string::npos)
                continue;

            auto& entry = info[line.substr(0, firstTab)];
            entry.foregroundProcess =
                line.substr(firstTab + 1, secondTab - firstTab - 1);
            entry.workingDirectory = line.substr(secondTab + 1);
        }
    }
}

void DaemonClient::requestInfo()
{
    if (isConnected() && !routes.empty())
        messenger->send(proto::make("info"));
}

void DaemonClient::spawn(const std::string& id,
                         const PtySize& size,
                         const std::string& cwd,
                         Routes routesToUse)
{
    routes[id] = std::move(routesToUse);
    messenger->send(proto::make("spawn",
                                {id, toString(size.cols), toString(size.rows)},
                                cwd));
}

void DaemonClient::write(const std::string& id, std::string_view data)
{
    messenger->send(proto::make("write", {id}, data));
}

void DaemonClient::resize(const std::string& id, const PtySize& size)
{
    messenger->send(
        proto::make("resize", {id, toString(size.cols), toString(size.rows)}));
}

void DaemonClient::kill(const std::string& id)
{
    routes.erase(id);
    messenger->send(proto::make("kill", {id}));
}

void DaemonClient::release(const std::string& id)
{
    routes.erase(id);
}

DaemonClient::ShellInfo DaemonClient::infoFor(const std::string& id) const
{
    if (auto found = info.find(id); found != info.end())
        return found->second;

    return {};
}

void DaemonClient::killServer()
{
    routes.clear();
    messenger->send(proto::make("quit-server"));
}

// ---- RemoteShell -----------------------------------------------------------

namespace
{
// A pane's shell living in the daemon. The pane talks to it exactly like a
// local PTY; detach() leaves the process running for the next attach.
class RemoteShell final : public Shell
{
public:
    explicit RemoteShell(std::string idToUse)
        : id(std::move(idToUse))
    {
    }

    ~RemoteShell() override
    {
        detach();
    }

    bool start(const PtyOptions& options,
               std::function<void(std::string)> onOutput,
               std::function<void()> onExit) override
    {
        auto* client = DaemonClient::get();

        if (client == nullptr || !client->isConnected())
            return false;

        auto routes = DaemonClient::Routes {};
        routes.onOutput = std::move(onOutput);
        routes.onExit = std::move(onExit);

        client->spawn(id, options.size, options.workingDirectory,
                      std::move(routes));
        attached = true;
        return true;
    }

    void write(std::string_view data) override
    {
        if (auto* client = DaemonClient::get())
            client->write(id, data);
    }

    void resize(const PtySize& size) override
    {
        if (auto* client = DaemonClient::get())
            client->resize(id, size);
    }

    std::string foregroundProcess() const override
    {
        if (auto* client = DaemonClient::get())
            return client->infoFor(id).foregroundProcess;

        return {};
    }

    std::string currentWorkingDirectory() const override
    {
        if (auto* client = DaemonClient::get())
            return client->infoFor(id).workingDirectory;

        return {};
    }

    void terminate() override
    {
        if (attached)
        {
            attached = false;

            if (auto* client = DaemonClient::get())
                client->kill(id);
        }
    }

    void detach() override
    {
        if (attached)
        {
            attached = false;

            if (auto* client = DaemonClient::get())
                client->release(id);
        }
    }

private:
    std::string id;
    bool attached = false;
};
} // namespace

std::unique_ptr<Shell> makeShell(const std::string& shellId)
{
    auto* client = DaemonClient::get();

    if (client != nullptr && client->isConnected())
        return std::make_unique<RemoteShell>(shellId);

    return std::make_unique<LocalShell>();
}
} // namespace term
