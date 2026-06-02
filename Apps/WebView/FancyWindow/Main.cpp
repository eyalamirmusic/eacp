#include <eacp/WebView/WebView.h>

using namespace eacp;
using namespace Graphics;

struct RootView final : View
{
    RootView() { addChildren({webView}); }

    void resized() override { scaleToFit({webView}); }

    WebView webView;
};

struct MyApp
{
    MyApp()
    {
        rootView.webView.loadURL("https://pond.audio/rick.mp4");
        window.setContentView(rootView);
    }

    // Mirrors the Electron window's titleBarStyle: 'hidden' + backgroundColor
    // + trafficLightPosition so the native window's chrome matches the web
    // app. A FullSizeContentView with a transparent, separator-less titlebar
    // lets the web app's own header render under the traffic lights as one
    // seamless black bar.
    static WindowOptions getOptions()
    {
        auto options = WindowOptions();

        options.width = 1200;
        options.height = 800;
        options.minWidth = 1200;
        options.minHeight = 800;

        options.flags.emplace_back(WindowFlags::FullSizeContentView);
        options.showTitle = false;

        options.titlebarTransparent = true;
        options.showTitlebarSeparator = false;
        options.trafficLightPosition = Point {10.f, 11.f};
        options.backgroundColor = Color::black();

        return options;
    }

    RootView rootView;
    Window window {getOptions()};
};

int main()
{
    eacp::Apps::run<MyApp>();
    return 0;
}
