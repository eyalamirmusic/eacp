#include <eacp/Graphics/Graphics.h>
#include <eacp/Network/Network.h>

#include <cstdio>
#include <optional>

// Two windows, two processes, one channel. The first instance claims the
// name, becomes the server and launches this same executable again; the
// second instance loses the claim, so it dials in as the client. A click
// in either window travels as a "dot <x> <y>" message in window-relative
// coordinates and appears as a dot in the other window.
//
// All the transport plumbing - reader thread, framing, main-thread
// delivery, teardown - lives in IPC::Messenger / IPC::MessageServer. The
// app only decides its role, wires callbacks and sends messages.
using namespace eacp;
using namespace Graphics;

namespace
{
constexpr auto channelName = "com.eacp.ipcdemo";
}

class Peer
{
public:
    // Claiming the name is the role decision: winner serves, loser dials.
    Peer()
    {
        try
        {
            server.emplace(channelName);
        }
        catch (const IPC::Error&)
        {
        }

        if (server)
        {
            server->onClient = [this](IPC::Messenger& session)
            {
                active = &session;
                wire(session);
                onConnected();
            };

            launchSecondInstance();
        }
        else
        {
            client.emplace(channelName);
            client->onConnected = [this] { onConnected(); };
            wire(*client);
        }
    }

    bool isServer() const { return server.has_value(); }

    void sendDot(Point relative)
    {
        if (auto* messenger = activeMessenger())
            messenger->send("dot " + std::to_string(relative.x) + " "
                            + std::to_string(relative.y));
    }

    Callback onConnected = [] {};
    std::function<void(Point)> onDot = [](Point) {};
    Callback onPeerLeft = [] {};

private:
    void wire(IPC::Messenger& messenger)
    {
        messenger.onMessage = [this](const std::string& message)
        {
            auto x = 0.f;
            auto y = 0.f;

            if (std::sscanf(message.c_str(), "dot %f %f", &x, &y) == 2)
                onDot({x, y});
        };

        messenger.onDisconnected = [this] { onPeerLeft(); };
    }

    IPC::Messenger* activeMessenger()
    {
        if (server)
            return active != nullptr && active->isConnected() ? active : nullptr;

        return client ? &*client : nullptr;
    }

    void launchSecondInstance()
    {
        auto& arguments = Apps::getAppEnvironment().commandLineArgs;

        if (arguments.size() == 0)
            return;

        auto options = Processes::ProcessOptions {};
        options.executable = arguments[0];
        options.captureOutput = false;
        child.emplace(std::move(options));
    }

    std::optional<IPC::MessageServer> server;
    std::optional<IPC::Messenger> client;
    IPC::Messenger* active = nullptr;

    std::optional<Processes::Process> child;
};

struct DemoView final : View
{
    DemoView(const std::string& roleName, Color roleColor, Color dotColorToUse)
        : dotColor(dotColorToUse)
    {
        getProperties().handlesMouseEvents = true;

        background->setFillColor({0.12f, 0.12f, 0.14f});
        dots->setFillColor(dotColor);

        title->setText(roleName);
        title->setFont(FontOptions().withName("Helvetica-Bold"));
        title->setColor(roleColor);
        status->setColor({0.75f, 0.75f, 0.78f});

        addChildren({background, dots, title, status});
    }

    void mouseDown(const MouseEvent& event) override
    {
        auto bounds = getLocalBounds();

        if (bounds.w > 0.f && bounds.h > 0.f)
            onClick({event.pos.x / bounds.w, event.pos.y / bounds.h});
    }

    void addDot(Point relative)
    {
        dotPoints.add(relative);
        rebuildDots();
    }

    void setStatus(const std::string& text) { status->setText(text); }

    void resized() override
    {
        auto bounds = getLocalBounds();

        auto backgroundPath = Path {};
        backgroundPath.addRect(bounds);
        background->setPath(backgroundPath);

        scaleToFit({background, dots, title, status});

        title->setPosition({20.f, bounds.h - 45.f});
        status->setPosition({20.f, bounds.h - 70.f});

        rebuildDots();
    }

    void rebuildDots()
    {
        auto bounds = getLocalBounds();
        auto path = Path {};

        for (auto& point: dotPoints)
            path.addEllipse(
                {point.x * bounds.w - 7.f, point.y * bounds.h - 7.f, 14.f, 14.f});

        dots->setPath(path);
    }

    std::function<void(Point)> onClick = [](Point) {};

    Color dotColor;
    Vector<Point> dotPoints;

    ShapeLayerView background;
    ShapeLayerView dots;
    TextLayerView title;
    TextLayerView status;
};

namespace
{
const auto serverColor = Color {0.35f, 0.65f, 1.f};
const auto clientColor = Color {1.f, 0.6f, 0.25f};

WindowOptions windowOptionsFor(bool isServer)
{
    auto options = WindowOptions {};
    options.title = isServer ? "IPC Demo - Server" : "IPC Demo - Client";
    options.initialPosition = isServer ? Point {120.f, 140.f} : Point {800.f, 140.f};
    return options;
}
} // namespace

struct IpcDemoApp
{
    IpcDemoApp()
        : view(peer.isServer() ? "Server" : "Client",
               peer.isServer() ? serverColor : clientColor,
               peer.isServer() ? clientColor : serverColor)
        , window(windowOptionsFor(peer.isServer()))
    {
        view.setStatus(peer.isServer() ? "Waiting for the second instance..."
                                       : "Connecting...");
        view.onClick = [this](Point relative) { peer.sendDot(relative); };

        peer.onConnected = [this]
        { view.setStatus("Connected - click anywhere to send"); };

        peer.onDot = [this](Point relative)
        {
            ++received;
            view.addDot(relative);
            view.setStatus("Received " + std::to_string(received)
                           + (received == 1 ? " click" : " clicks"));
        };

        peer.onPeerLeft = [this]
        {
            view.setStatus(peer.isServer() ? "Peer left - waiting for a new one"
                                           : "Peer left");
        };

        window.setContentView(view);
    }

    Peer peer;
    DemoView view;
    Window window;
    int received = 0;
};

int main(int argc, char* argv[])
{
    return Apps::run<IpcDemoApp>(argc, argv);
}
