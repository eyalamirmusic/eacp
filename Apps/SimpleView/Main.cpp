#include <eacp/WebView/WebView.h>

#include <memory>

using namespace eacp;
using namespace Graphics;

struct PopupWindow final
{
    PopupWindow(std::unique_ptr<WebView> popup,
                std::function<void(PopupWindow*)> onClose)
        : webView(std::move(popup))
        , closeHandler(std::move(onClose))
    {
        webView->onClose = [this]()
        {
            if (closeHandler)
                closeHandler(this);
        };

        window.setContentView(*webView);
    }

    std::unique_ptr<WebView> webView;
    std::function<void(PopupWindow*)> closeHandler;
    Window window;
};

struct ParentView final : View
{
    ParentView()
    {
        webView.loadURL("https://dev.tamber.ai/app/pro/sonic-atlas");
        webView.onNewWindowRequested =
            [this](std::unique_ptr<WebView> popup, const std::string&)
        {
            openPopup(std::move(popup));
            return true;
        };
        addChildren({webView});
    }

    void resized() override { scaleToFit({webView}); }

    void openPopup(std::unique_ptr<WebView> popupWebView)
    {
        auto popup = std::make_unique<PopupWindow>(
            std::move(popupWebView),
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
