#include "Types.h"

#include <eacp/WebView/WebView.h>

using namespace eacp;
using namespace Graphics;

struct MyApp
{
    MyApp()
    {
        transport.getBridge().use(params);

        setApplicationMenuBar(buildDefaultWebViewMenuBar(), window);
        window.setContentView(webView);
    }

    // Declared before the transport, so it outlives the bridge's listeners.
    Api::ParametersApi params;
    WebView webView {embeddedOptions("WebApp")};
    WebViewBridge transport {webView};
    Window window;
    Threads::Timer timer {[this] { params.advanceTick(); }, 30};
};

int main()
{
    return eacp::Apps::run<MyApp>();
}
