#include <eacp/WebView/WebView.h>

using namespace eacp;
using namespace Graphics;

// A minimal browser demonstrating the window chrome and browser options:
//
//   eacp_set_app_icon (CMakeLists.txt)  bakes Icon.icns / Icon.ico into
//       the bundle / executable — Finder, Explorer, the Dock and the
//       taskbar all show it, at rest and while running, with no runtime
//       code
//   WindowOptions::altTabIcon           the blue icon everywhere, except
//       the Windows Alt-Tab switcher, which shows this orange override
//   WebView::Options::statusBar         off, so hovering a link shows no
//       URL overlay — the same behaviour as WKWebView
//   WebView::Options::applicationNameForUserAgent
//       adds Safari's version to the end of the user agent, so sites that
//       look for Safari serve this window the pages they serve Safari
//       (macOS and iOS)
//   WebView::Options::backForwardGestures
//       a two-finger swipe goes back or forward through the pages visited
//       (macOS and iOS)
//   WebView::Options::pinchToZoom
//       a trackpad pinch zooms the page (macOS)
//   WebView::Options::elementFullscreen
//       a page's fullscreen button, such as a video player's, fills the
//       screen (macOS 12.3+ and iOS 15.4+)
//
// Type a URL in the address bar and press return to navigate.

struct BrowserView final : View
{
    BrowserView()
    {
        addressBar.onSubmit([this](const std::string& text)
                            { webView.loadURL(withScheme(text)); });

        webView.loadURL(homePage);
        addChildren({addressBar, webView});
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        addressBar.setBounds(bounds.removeFromTop(44.f).inset(8.f, 7.f));
        webView.setBounds(bounds);
    }

    static std::string withScheme(const std::string& url)
    {
        if (url.find("://") != std::string::npos)
            return url;

        return "https://" + url;
    }

    static WebView::Options getWebViewOptions()
    {
        auto options = WebView::Options();
        options.statusBar = false;
        options.applicationNameForUserAgent = safariUserAgentTail;
        options.backForwardGestures = true;
        options.pinchToZoom = true;
        options.elementFullscreen = true;
        return options;
    }

    static constexpr auto homePage = "https://www.wikipedia.org";
    static constexpr auto safariUserAgentTail = "Version/26.0 Safari/605.1.15";

    TextInput addressBar {std::string(homePage)};
    WebView webView {getWebViewOptions()};
};

Image decodeIcon(const std::string& name)
{
    auto png = ResEmbed::get(name, "Browser");
    return Image::decode(png.data(), png.getSize());
}

WindowOptions windowOptions()
{
    auto options = WindowOptions();

    options.title = "EACP Browser";
    options.width = 1100;
    options.height = 760;
    options.minWidth = 480;
    options.minHeight = 320;

    options.altTabIcon = [] { return decodeIcon("AltTabIcon.png"); };

    return options;
}

int main()
{
    return Graphics::runWindowedApp<BrowserView>(windowOptions());
}
