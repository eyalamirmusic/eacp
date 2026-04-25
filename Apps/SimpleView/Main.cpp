#include <eacp/WebView/WebView.h>

using namespace eacp;
using namespace Graphics;

struct ParentView final : View
{
    ParentView()
    {
        webView.loadURL("https://dev.tamber.ai/app/pro/sonic-atlas");
        addChildren({webView});
    }

    void resized() override { scaleToFit({webView}); }

    WebView webView;
};

struct MyApp
{
    MyApp() { window.setContentView(parentView); }

    ParentView parentView;
    Window window;
};

int main()
{
    eacp::Apps::run<MyApp>();

    return 0;
}
