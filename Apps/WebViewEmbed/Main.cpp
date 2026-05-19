#include "Types.h"

#include <eacp/Core/Threads/Timer.h>
#include <eacp/WebView/WebView.h>

using namespace eacp;
using namespace Graphics;

struct MyApp
{
    MyApp()
    {
        setApplicationMenuBar(buildDefaultWebViewMenuBar());
        window.setContentView(webView);
    }

    WebView webView {embeddedOptions("WebApp")};
    WebViewBridge transport {webView};
    Window window;
    Listener bridgeBinding =
        bindToBridge(parametersState(), transport.getBridge(), "parameters");
    Threads::Timer timer {[] { advanceTick(); }, 30};
};

int main()
{
    eacp::Apps::run<MyApp>();

    return 0;
}
