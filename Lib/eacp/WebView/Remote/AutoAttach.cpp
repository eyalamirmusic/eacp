#include "AutoAttach.h"

#include "DebugServer.h"

#include <eacp/Core/Utils/Environment.h>
#include <eacp/Core/Utils/Logging.h>
#include <eacp/Core/Utils/Strings.h>
#include <eacp/WebView/WebView/DebugAttach.h>

#include <utility>

namespace eacp::WebView::Remote
{

namespace
{

struct ServerAttachment : Graphics::Detail::DebugAttachment
{
    explicit ServerAttachment(OwningPointer<DebugServer> serverToUse)
        : server(std::move(serverToUse))
    {
    }

    OwningPointer<DebugServer> server;
};

OwningPointer<DebugServer>
    makeServer(Graphics::WebView& webView, Miro::Bridge& bridge, int port)
{
    auto result = OwningPointer<DebugServer> {};

    try
    {
        result.create(webView, bridge, DebugServerOptions {.port = port});
    }
    catch (const std::exception&)
    {
        // Most likely the port is taken — another debug-enabled app
        // (or a second instance of this one). An ephemeral port keeps
        // every instance reachable; port() / the LOG line say where.
        LOG("DebugServer: port " + std::to_string(port)
            + " unavailable, falling back to an ephemeral port");
        result.create(webView, bridge, DebugServerOptions {.port = 0});
    }

    return result;
}

} // namespace

std::optional<int> resolveDebugPort(const std::optional<std::string>& value)
{
    if (!value)
        return defaultDebugPort;

    if (Strings::equalsCaseInsensitive(*value, "off"))
        return std::nullopt;

    return Strings::parseIntOr(*value, defaultDebugPort);
}

bool installAutoAttach()
{
    Graphics::Detail::debugAttachFactory() =
        [](Graphics::WebView& webView,
           Miro::Bridge& bridge) -> OwningPointer<Graphics::Detail::DebugAttachment>
    {
        auto port = resolveDebugPort(getEnv("EACP_DEBUG_PORT"));
        if (!port)
            return {};

        try
        {
            auto attachment = OwningPointer<Graphics::Detail::DebugAttachment> {};
            attachment.create<ServerAttachment>(makeServer(webView, bridge, *port));
            return attachment;
        }
        catch (const std::exception& e)
        {
            // A broken debug affordance must never take the app down.
            LOG(std::string {"DebugServer: failed to start: "} + e.what());
            return {};
        }
    };

    return true;
}

} // namespace eacp::WebView::Remote
