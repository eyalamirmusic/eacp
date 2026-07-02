#include <eacp/WebView/WebView.h>

#include <eacp/Graphics/Widgets/TextInput.h>
#include <ResEmbed/ResEmbed.h>

using namespace eacp;
using namespace Graphics;

// A minimal browser demonstrating the window chrome options:
//
//   WindowOptions::applicationIcon   decodes the PNG embedded with ResEmbed
//       (CMakeLists.txt) — the Windows taskbar / Alt-Tab switcher and the
//       macOS Dock show it instead of the generic application icon
//   WebView::Options::statusBar      off, so hovering a link shows no URL
//       overlay — the same behaviour as WKWebView
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

        options.applicationIcon = []
        {
            auto png = ResEmbed::get("Icon.png", "Browser");
            return Image::decode(png.data(), png.getSize());
        };

        return options;
    }

    BrowserView view;
    Window window {getOptions()};
};

int main()
{
    eacp::Apps::run<BrowserApp>();
    return 0;
}
