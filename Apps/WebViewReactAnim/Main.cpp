#include <eacp/Core/Threads/Timer.h>
#include <eacp/WebView/WebView.h>
#include <Miro/Miro.h>
#include <chrono>
#include <cmath>

using namespace eacp;
using namespace Graphics;

namespace
{
struct Tick
{
    double angle = 0.0;

    MIRO_REFLECT(angle)
};
} // namespace

struct MyApp
{
    using Clock = std::chrono::steady_clock;

    MyApp()
    {
        setApplicationMenuBar(buildDefaultWebViewMenuBar());
        window.setContentView(webView);
    }

    Tick currentTick() const
    {
        auto seconds = std::chrono::duration<double>(Clock::now() - start).count();
        return {.angle = std::fmod(seconds * 90.0, 360.0)};
    }

    Clock::time_point start = Clock::now();
    WebView webView {embeddedOptions("ReactAnimApp")};
    WebViewBridge transport {webView};
    Window window;
    Threads::Timer timer {[this] { transport.getBridge().emit("tick", currentTick()); }, 120};
};

int main()
{
    eacp::Apps::run<MyApp>();
    return 0;
}
