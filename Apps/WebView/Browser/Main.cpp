#include <eacp/WebView/WebView.h>

using namespace eacp;
using namespace Graphics;

// A minimal browser demonstrating the window chrome options:
//
//   eacp_set_app_icon (CMakeLists.txt)  bakes Icon.icns / Icon.ico into
//       the bundle / executable — Finder, Explorer, the Dock and the
//       taskbar all show it, at rest and while running, with no runtime
//       code
//   WindowOptions::altTabIcon           the blue icon everywhere, except
//       the Windows Alt-Tab switcher, which shows this orange override
//   WebView::Options::statusBar         off, so hovering a link shows no
//       URL overlay — the same behaviour as WKWebView
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
        return options;
    }

    static constexpr auto homePage = "https://www.wikipedia.org";

    TextInput addressBar {std::string(homePage)};
    WebView webView {getWebViewOptions()};
};

struct BrowserApp
{
    BrowserApp() { window.setContentView(view); }

    static WindowOptions getOptions()
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

    static Image decodeIcon(const std::string& name)
    {
        auto png = ResEmbed::get(name, "Browser");
        return Image::decode(png.data(), png.getSize());
    }

    BrowserView view;
    Window window {getOptions()};
};

int main()
{
    eacp::Apps::run<BrowserApp>();
    return 0;
}
