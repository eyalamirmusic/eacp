#pragma once

#include <eacp/Graphics/Remote/WindowDebugServer.h>
#include <eacp/WebView/Test/AppDriver.h>

#include <functional>
#include <string>

namespace Miro
{
class Bridge;
}

namespace eacp::Graphics
{
class WebView;
}

namespace eacp::WebView::Remote
{

// The WebView debug capability as a window-server extension: it adds the
// DOM-driving tools (list_elements, click, fill, press, console_logs,
// invoke_command, navigate, snapshot, ...) plus page console capture onto
// the WindowDebugServer. Screenshot/recording stay on the window server
// itself (window-level, every app), so this only adds DOM driving when a
// WebView is present — one app, one endpoint, capture + DOM.
//
// Apps don't construct this: the WebView auto-attach hook (AutoAttach.h)
// creates one per bridge and hands it to the window server. Tests make it
// directly.
class WebViewTools : public Graphics::Remote::ServerExtension
{
public:
    WebViewTools(Graphics::WebView& webViewToUse,
                 Miro::Bridge& bridgeToUse,
                 std::string snapshotDir = {});
    ~WebViewTools() override;

    WebViewTools(const WebViewTools&) = delete;
    WebViewTools& operator=(const WebViewTools&) = delete;

    void registerTools(Graphics::Remote::WindowDebugServer& server) override;

private:
    void installConsoleCapture();

    Graphics::WebView& webView;
    Test::AppDriver driver;
    std::function<void(const std::string&)> previousFinishedHandler;
};

} // namespace eacp::WebView::Remote
