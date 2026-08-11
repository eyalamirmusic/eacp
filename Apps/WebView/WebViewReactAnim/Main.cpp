#include "Types.h"

#include <eacp/WebView/WebView.h>

using namespace eacp;
using namespace Graphics;

struct MyApp
{
    MyApp()
    {
        transport.getBridge().use(clock);

        setApplicationMenuBar(buildDefaultWebViewMenuBar(), window);
        window.setContentView(webView);
    }

    // Declared before the transport, so it outlives the bound listeners.
    Api::Clock clock;
    WebView webView {embeddedOptions("ReactAnimApp")};
    WebViewBridge transport {webView};
    Window window;
    Threads::Timer timer {[&] { clock.update(); }, 120};
};

int main()
{
    return eacp::Apps::run<MyApp>();
}
