#include <eacp/Core/Shell/Shell.h>
#include <eacp/WebView/WebView.h>

#include <memory>
#include <vector>
#include "Content.hpp"

using namespace eacp;
using namespace Graphics;


struct WebViewContainer final : View
{
    WebViewContainer() { addChildren({webView}); }
    void resized() override { scaleToFit({webView}); }
    WebView webView;
};

struct PopupWindow
{
    PopupWindow() : window(popupOptions()) { window.setContentView(view); }

    static WindowOptions popupOptions()
    {
        WindowOptions options;
        options.onClose = []() {};
        return options;
    }

    WebViewContainer view;
    Window window;
};

struct DemoApp
{
    DemoApp()
    {
        mainWindow.setContentView(mainView);
        mainView.webView.loadHTML(kHomePage, "https://demo.eacp.local/");

        mainView.webView.onNavigationDecision =
            [](const NavigationRequest& request)
        {
            if (isSameOrigin(request.url))
            {
                return NavigationDecision::Allow;
            }
            return NavigationDecision::OpenExternally;
        };

        mainView.webView.onNewWindowRequested =
            [this](const NewWindowRequest&) -> WebView*
        {
            popups.push_back(std::make_unique<PopupWindow>());
            return &popups.back()->view.webView;
        };
    }

    WebViewContainer mainView;
    Window mainWindow;
    std::vector<std::unique_ptr<PopupWindow>> popups;
};

int main()
{
    eacp::Apps::run<DemoApp>();
    return 0;
}
