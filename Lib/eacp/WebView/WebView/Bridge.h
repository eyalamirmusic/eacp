#pragma once

#include "WebView.h"

#include <Miro/Miro.h>

#include <string>

namespace eacp::Graphics
{

using EmptyMessage = Miro::EmptyValue;

// Transport adapter that routes WebView <-> C++ messages through a
// Miro::Bridge. The Bridge owns the CommandTable and the event
// registry; this class is responsible only for the WebView wire
// format (script message handler + injected JS shim + an event
// broadcaster registered on construction and removed on destruction).
//
// The Bridge can be shared with other transports (e.g. an
// HTTP::Rpc::Server) so a single set of typed handlers — including
// those declared via MIRO_EXPORT_COMMAND — is served over multiple
// wires concurrently.
class WebViewBridge
{
public:
    WebViewBridge(WebView& webView);
    ~WebViewBridge();

    Miro::Bridge& getBridge() { return bridge; }

private:
    void onMessage(const std::string& body);
    void deliver(double id,
                 const Miro::Json::Value& result,
                 const std::string* error);
    void broadcast(std::string_view event, const Miro::Json::Value& payload);

    WebView& webView;
    Miro::Bridge bridge;
    Miro::Bridge::Subscription subscription;
};

} // namespace eacp::Graphics
