#include "Types.h"

#include <eacp/WebView/WebView.h>

using namespace eacp;
using namespace Graphics;

struct MyApp
{
    MyApp()
    {
        transport.getBridge().use(params);

        setApplicationMenuBar(buildDefaultWebViewMenuBar());
        window.setContentView(webView);
    }

    // params declared first → destructed last (after the transport's
    // bridge has torn down its listeners and handlers).
    Api::ParametersApi params;
    WebView webView {embeddedOptions("WebApp")};
    WebViewBridge transport {webView};
    Window window;
    Threads::Timer timer {[this] { params.advanceTick(); }, 30};
};

int main()
{
    eacp::Apps::run<MyApp>();
    return 0;
}
