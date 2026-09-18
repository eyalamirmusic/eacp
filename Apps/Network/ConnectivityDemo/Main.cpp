#include <eacp/Graphics/Graphics.h>
#include <eacp/Network/Network.h>

#include <ctime>

using namespace eacp;
using namespace Graphics;

namespace Connectivity = Network::Connectivity;

// Network::Connectivity::Monitor, and the one listener an app needs to
// follow it.
//
// Turn Wi-Fi off and the headline flips to red before the menu bar icon has
// finished animating; turn it back on and it flips back. Nothing is polled
// and nothing is fetched: the OS pushes its own view of the machine's routes
// (Network.framework here, the connectivity hint on Windows, netlink on
// Linux) and the monitor triggers.
//
// The listener is an EA::Listener member, built last so the view it refreshes
// already exists. Its default mode, TriggerNow, calls refresh() once as it is
// constructed - which is how the window opens showing the state it joined
// rather than an empty one - and the broadcaster calls it again on every
// change after that, on the message thread. The trigger carries nothing: the
// state is pulled from Monitor::get().getState() each time.

struct StatusRoot final : View
{
    void refresh()
    {
        auto newState = Connectivity::getState();

        if (hasState && !(newState == state))
            lastChangeTime = timeOfDay();

        state = newState;
        hasState = true;
        repaint();
    }

    void paint(Context& g) override
    {
        auto bounds = getLocalBounds();
        auto middle = bounds.h * 0.5f;

        g.setColor(background);
        g.fillRect(bounds);

        g.setColor(state.online ? connected : disconnected);
        drawCentred(g,
                    state.online ? "Connected to the internet"
                                 : "Not connected to the internet",
                    middle,
                    headlineFont);

        g.setColor(Color::white(0.6f));
        drawCentred(g, describeLink(), middle + 34.f, detailFont);

        g.setColor(Color::white(0.35f));
        drawCentred(g, describeLastChange(), middle + 56.f, detailFont);
    }

private:
    static std::string timeOfDay()
    {
        auto now = std::time(nullptr);
        const auto* local = std::localtime(&now);

        if (local == nullptr)
            return {};

        char buffer[16] = {};
        std::strftime(buffer, sizeof(buffer), "%H:%M:%S", local);

        return buffer;
    }

    std::string describeLink() const
    {
        return Connectivity::toString(state.interfaceKind) + "   -   "
               + (state.expensive ? "expensive" : "not expensive") + "   -   "
               + (state.constrained ? "constrained" : "not constrained");
    }

    std::string describeLastChange() const
    {
        if (lastChangeTime.empty())
            return "no change since launch";

        return "last change at " + lastChangeTime;
    }

    void drawCentred(Context& g, const std::string& text, float y, const Font& font)
    {
        auto width = TextMetrics::measureWidth(text, font);
        g.drawText(text, {(getLocalBounds().w - width) * 0.5f, y}, font);
    }

    Connectivity::State state;
    bool hasState = false;
    std::string lastChangeTime;

    Font headlineFont {FontOptions().withName("Helvetica-Bold").withSize(28.f)};
    Font detailFont {FontOptions().withName("Helvetica").withSize(13.f)};

    Color background {0.11f, 0.12f, 0.15f, 1.f};
    Color connected {0.36f, 0.82f, 0.47f, 1.f};
    Color disconnected {0.93f, 0.38f, 0.38f, 1.f};
};

struct ConnectivityDemoApp
{
    static WindowOptions getOptions()
    {
        auto options = WindowOptions();

        options.title = "Connectivity";
        options.width = 620;
        options.height = 260;
        options.backgroundColor = Color {0.11f, 0.12f, 0.15f, 1.f};

        return options;
    }

    StatusRoot root;
    Window window {root, getOptions()};

    EA::Listener listener {Connectivity::Monitor::get(), [this] { root.refresh(); }};
};

int main()
{
    return eacp::Apps::run<ConnectivityDemoApp>();
}
