#include <eacp/WebView/WebView.h>
#include <WebResources.h>

#include <ea_data_structures/Pointers/OwningPointer.h>
#include <ea_data_structures/Structures/OwnedVector.h>

using namespace eacp;
using namespace Graphics;

struct PopupWindow final
{
    PopupWindow(EA::OwningPointer<WebView> popup,
                std::function<void(PopupWindow*)> onClose)
        : closeHandler(std::move(onClose))
        , webView(std::move(popup))
        , window(makeWindowOptions())
    {
        webView->onClose = [this]() { requestClose(); };
        window.setContentView(*webView);
    }

    std::function<void(PopupWindow*)> closeHandler;
    EA::OwningPointer<WebView> webView;
    bool closing = false;
    Window window;

private:
    WindowOptions makeWindowOptions()
    {
        auto opts = WindowOptions {};
        opts.title = "Child window";
        opts.width = 420;
        opts.height = 320;
        opts.isPrimary = false;
        opts.onQuit = [this]() { requestClose(); };
        return opts;
    }

    void requestClose()
    {
        if (closing)
            return;
        closing = true;
        if (closeHandler)
            closeHandler(this);
    }
};

struct ParentView final : View
{
    ParentView()
    {
        webView.onNewWindowRequested =
            [this](EA::OwningPointer<WebView> popup, const std::string&)
        {
            openPopup(std::move(popup));
            return true;
        };
        addChildren({webView});
    }

    void resized() override { scaleToFit({webView}); }

    void openPopup(EA::OwningPointer<WebView> popupWebView)
    {
        popups.createNew(
            std::move(popupWebView),
            [this](PopupWindow* p)
            { Threads::callAsync([this, p]() { closePopup(p); }); });
    }

    void closePopup(PopupWindow* popup)
    {
        popups.removeItem(*popup);
    }

    WebView webView {embeddedOptions("EmbeddedPopupsApp")};
    EA::OwnedVector<PopupWindow> popups;
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
