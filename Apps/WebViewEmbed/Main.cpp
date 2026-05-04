#include "Types.h"

#include <eacp/WebView/WebView.h>

#include <chrono>

using namespace eacp;
using namespace Graphics;

namespace
{
long long currentEpochMillis()
{
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

PingResponse handlePing(const EmptyMessage&)
{
    return PingResponse {.pong = true, .serverTimeMs = currentEpochMillis()};
}
} // namespace

struct MyApp
{
    MyApp()
    {
        setApplicationMenuBar(buildDefaultWebViewMenuBar());
        window.setContentView(webView);
        bridge.on("ping", handlePing);
    }

    WebView webView {embeddedOptions("WebApp")};
    WebViewBridge bridge {webView};
    Window window;
};

int main()
{
    eacp::Apps::run<MyApp>();

    return 0;
}
