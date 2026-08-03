#include <eacp/Network/IPCRpc/RpcClient.h>
#include <eacp/Network/IPCRpc/RpcServer.h>
#include <eacp/UI/UI.h>

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

// Every dot the peer has sent, drawn as one component.
//
// One filled ellipse per dot, and no path at all: a circle this size is a
// rounded rectangle whose corner radius is half its side, which the shape
// renderer draws from the same distance field as every other rectangle in the
// tree. So a hundred dots are a hundred quads in one instanced draw, and adding
// one costs a repaint rather than a re-rasterized path.
struct DotField final : UI::Component
{
    explicit DotField(const UI::Color& dotColourToUse)
        : dotColour(dotColourToUse)
    {
        setInterceptsMouseClicks(true);
    }

    void mouseDown(const UI::MouseEvent& event) override
    {
        auto bounds = getLocalBounds();

        if (bounds.w > 0.f && bounds.h > 0.f)
            onClick({event.position.x / bounds.w, event.position.y / bounds.h});
    }

    void addDot(UI::Point relative)
    {
        dots.add(relative);
        repaint();
    }

    void paint(UI::Graphics& g) override
    {
        auto bounds = getLocalBounds();

        g.setColour(dotColour);

        for (auto& dot: dots)
            g.fillRoundedRect(
                {dot.x * bounds.w - 7.f, dot.y * bounds.h - 7.f, 14.f, 14.f}, 7.f);
    }

    std::function<void(UI::Point)> onClick = [](UI::Point) {};

    UI::Color dotColour;
    Vector<UI::Point> dots;
};

struct DemoContent final : UI::Component
{
    DemoContent(const std::string& roleName,
                const UI::Color& roleColour,
                const UI::Color& dotColour)
        : field(dotColour)
    {
        title.setText(roleName);
        title.setFontStyle(UI::FontStyle::Bold);
        title.setFontSize(15.f);
        title.setColour(roleColour);

        status.setColour({0.75f, 0.75f, 0.78f, 1.f});

        addChildren({field, title, status});
    }

    void setStatus(const std::string& text) { status.setText(text); }

    void paint(UI::Graphics& g) override { g.fillAll({0.12f, 0.12f, 0.14f, 1.f}); }

    void resized() override
    {
        field.setBounds(getLocalBounds());

        auto header = getLocalBounds().inset(20.f, 16.f);
        title.setBounds(header.removeFromTop(24.f));
        status.setBounds(header.removeFromTop(22.f));
    }

    DotField field;
    UI::Label title;
    UI::Label status;
};

struct DemoHost final : UI::ComponentHost
{
    DemoHost(const std::string& roleName,
             const UI::Color& roleColour,
             const UI::Color& dotColour)
        : content(roleName, roleColour, dotColour)
    {
        setBackgroundColour({0.12f, 0.12f, 0.14f, 1.f});
        setRootComponent(content);
    }

    DemoContent content;
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
        : host(peer.isServer() ? "Server" : "Client",
               peer.isServer() ? serverColor : clientColor,
               peer.isServer() ? clientColor : serverColor)
        , window(windowOptionsFor(peer.isServer()))
    {
        auto& content = host.content;

        content.setStatus(peer.isServer() ? "Waiting for the second instance..."
                                          : "Connecting...");
        content.field.onClick = [this](Point relative) { peer.sendDot(relative); };

        peer.onConnected = [this]
        { host.content.setStatus("Connected - click anywhere to send"); };

        peer.onDot = [this](Point relative)
        {
            ++received;
            host.content.field.addDot(relative);
            host.content.setStatus("Received " + std::to_string(received)
                                   + (received == 1 ? " click" : " clicks"));
        };

        peer.onTotal = [this](int total)
        {
            host.content.setStatus("Server now holds " + std::to_string(total)
                                   + (total == 1 ? " dot" : " dots"));
        };

        peer.onPeerLeft = [this]
        {
            host.content.setStatus(
                peer.isServer() ? "Peer left - waiting for a new one" : "Peer left");
        };

        window.setContentView(host);
    }

    Peer peer;
    DemoHost host;
    Window window;
    int received = 0;
};

int main(int argc, char* argv[])
{
    return Apps::run<IpcDemoApp>(argc, argv);
}
