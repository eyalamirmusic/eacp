#include "AutoAttach.h"

#include "WebViewTools.h"

#include <eacp/Core/Utils/Logging.h>
#include <eacp/Graphics/Remote/WindowAutoAttach.h>
#include <eacp/Graphics/Remote/WindowDebugServer.h>
#include <eacp/WebView/WebView/DebugAttach.h>

#include <utility>

namespace eacp::WebView::Remote
{

bool installAutoAttach()
{
    Graphics::Detail::webViewDebugHook() =
        [](Graphics::WebView& webView, Miro::Bridge& bridge)
    {
        try
        {
            // Hand the DOM tools to the app's window server — adopted now,
            // or the moment the server is created (the Window and bridge can
            // be constructed in either order). The server owns them.
            auto tools = OwningPointer<Graphics::Remote::ServerExtension> {};
            tools.create<WebViewTools>(webView, bridge);
            Graphics::Remote::attachExtensionOrDefer(std::move(tools));
        }
        catch (const std::exception& e)
        {
            // A broken debug affordance must never take the app down.
            LOG(std::string {"WebViewTools: failed to attach: "} + e.what());
        }
    };

    return true;
}

} // namespace eacp::WebView::Remote
