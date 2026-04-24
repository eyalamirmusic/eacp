#include <eacp/WebView/WebView.h>
#include <WebResources.h>

using namespace eacp;
using namespace Graphics;

WebView::EmbeddedOptions getOptions()
{
    return {.provider = fromResEmbed("WebApp")};
}

struct MyApp
{
    MyApp() { window.setContentView(webView); }

    WebView webView{getOptions()};
    Window window;
};

int main()
{
    eacp::Apps::run<MyApp>();

    return 0;
}