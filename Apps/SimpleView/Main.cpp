#include <eacp/WebView/WebView.h>

#include <memory>

using namespace eacp;
using namespace Graphics;

struct PopupWindow final
{
    PopupWindow(const std::string& url, std::function<void(PopupWindow*)> onClose)
        : closeHandler(std::move(onClose))
    {
        webView.loadURL(url);
        webView.onClose = [this]()
        {
            if (closeHandler)
                closeHandler(this);
        };

        window.setContentView(webView);
    }

    std::function<void(PopupWindow*)> closeHandler;
    WebView webView;
    Window window;
};

struct ParentView final : View
{
    ParentView()
    {
        webView.loadURL("https://dev.tamber.ai/app/pro/sonic-atlas");
        webView.onNewWindowRequested = [this](const std::string& url)
        {
            openPopup(url);
            return true;
        };
        addChildren({webView});
    }

    void resized() override { scaleToFit({webView}); }

    void openPopup(const std::string& url)
    {
        auto popup = std::make_unique<PopupWindow>(
            url,
            [this](PopupWindow* p)
            { Threads::callAsync([this, p]() { closePopup(p); }); });
        popups.push_back(std::move(popup));
    }

    void closePopup(PopupWindow* popup)
    {
        std::erase_if(popups, [popup](const auto& p) { return p.get() == popup; });
    }

    WebView webView;
    std::vector<std::unique_ptr<PopupWindow>> popups;
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
