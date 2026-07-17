#include <eacp/Graphics/Graphics.h>
#include <eacp/Network/IPCRpc/RpcClient.h>
#include <eacp/Network/IPCRpc/RpcServer.h>

#include <optional>

// Two windows, two processes, one typed RPC bridge. The first instance
// claims the name, becomes the server and launches this same executable
// again; the second instance loses the claim, so it dials in as the
// client. Clicks travel typed: the client invokes the server's "addDot"
// command and hears back how many dots the server holds; the server
// pushes its own clicks to the client as "dot" events. Both directions
// are Miro-serialized structs - no strings are parsed anywhere.
using namespace eacp;
using namespace Graphics;

struct Dot
{
    float x = 0;
    float y = 0;

    MIRO_REFLECT(x, y)
};

struct DotTotal
{
    int total = 0;

    MIRO_REFLECT(total)
};

// The server's API, mounted on the bridge: one typed command in, a typed
// reply out. Handlers run main-thread-deferred by default, so touching
// app state from here is safe.
class DotApi
{
public:
    void reflect(Miro::ApiReflector& r) { r.command(&DotApi::addDot, "addDot"); }

    DotTotal addDot(const Dot& dot)
    {
        onDot(dot);
        return {++total};
    }

    std::function<void(const Dot&)> onDot = [](const Dot&) {};
    int total = 0;
};

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
        api.onDot = [this](const Dot& dot) { onDot({dot.x, dot.y}); };
        bridge.use(api);

        try
        {
            server.emplace(channelName, bridge);
        }
        catch (const IPC::Error&)
        {
        }

        if (server)
        {
            server->onClientConnected = [this] { onConnected(); };
            server->onClientDisconnected = [this] { onPeerLeft(); };
            launchSecondInstance();
        }
        else
        {
            client.emplace(channelName);
            client->onConnected = [this] { onConnected(); };
            client->onDisconnected = [this] { onPeerLeft(); };
            client->on<Dot>("dot",
                            [this](const Dot& dot) { onDot({dot.x, dot.y}); });
        }
    }

    bool isServer() const { return server.has_value(); }

    // The two directions showcase the two primitives: a client invokes a
    // typed command and learns the server's new total from the reply; the
    // server pushes an event to every connected client.
    void sendDot(Point relative)
    {
        auto dot = Dot {relative.x, relative.y};

        if (server)
        {
            bridge.emit("dot", dot);
            return;
        }

        client->call<DotTotal>("addDot", dot)
            .then([this](DotTotal reply) { onTotal(reply.total); },
                  [](const std::string&) {});
    }

    Callback onConnected = [] {};
    std::function<void(Point)> onDot = [](Point) {};
    std::function<void(int)> onTotal = [](int) {};
    Callback onPeerLeft = [] {};

private:
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

    DotApi api;
    Miro::Bridge bridge;
    std::optional<IPC::RpcServer> server;
    std::optional<IPC::RpcClient> client;

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

        peer.onTotal = [this](int total)
        {
            view.setStatus("Server now holds " + std::to_string(total)
                           + (total == 1 ? " dot" : " dots"));
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
