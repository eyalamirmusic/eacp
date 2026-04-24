#include <eacp/WebView/WebView.h>

#include "Content.hpp"

using namespace eacp;
using namespace Graphics;

struct DemoApp
{
    DemoApp()
        : main(windows.emplaceBackWebView())
    {
        main.webView.loadHTML(kHomePage, "https://demo.eacp.local/");
        main.window.setTitle("Demo App");

        main.webView.onNavigationDecision = [](const NavigationRequest& request)
        {
            if (isSameOrigin(request.url))
                return NavigationDecision::Allow;

            // or do some other checks, e.g. if the URL is in a whitelist of
            // allowed external URLs

            return NavigationDecision::OpenExternally;
        };

        // close children when the parent closes
        main.window.onWillClose = [this]
        {
            windows.closeChildrenOf(main.window);
        };
    }

    WindowPool windows;
    WindowedWebView main;
};

int main()
{
    eacp::Apps::run<DemoApp>();
    return 0;
}
