#include <eacp/WebView/WebView.h>
#include <WebResources.h>

using namespace eacp;
using namespace Graphics;

struct MyApp
{
    MyApp() { window.setContentView(webView); }

    WebView webView {embeddedOptions("WebApp")};
    Window window;
};

int main()
{
    eacp::Apps::run<MyApp>();

    return 0;
}