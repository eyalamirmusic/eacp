#include "WindowAutoAttach.h"

#include "WindowDebugServer.h"

#include <eacp/Core/Utils/Environment.h>
#include <eacp/Core/Utils/Logging.h>
#include <eacp/Core/Utils/Strings.h>
#include <eacp/Graphics/Helpers/DebugAttach.h>

#include <utility>

namespace eacp::Graphics::Remote
{

namespace
{

// One server per app, bound to the primary (first) window. The
// attachment owns it; tearing the attachment down clears the pointer.
WindowDebugServer* gCurrentServer = nullptr;

// Extensions handed in before the server existed, adopted on creation.
Vector<OwningPointer<ServerExtension>>& pendingExtensions()
{
    static auto pending = Vector<OwningPointer<ServerExtension>> {};
    return pending;
}

struct ServerAttachment : Graphics::Detail::DebugAttachment
{
    explicit ServerAttachment(OwningPointer<WindowDebugServer> serverToUse)
        : server(std::move(serverToUse))
    {
    }

    ~ServerAttachment() override { gCurrentServer = nullptr; }

    OwningPointer<WindowDebugServer> server;
};

OwningPointer<WindowDebugServer> makeServer(Graphics::Window& window, int port)
{
    auto result = OwningPointer<WindowDebugServer> {};

    try
    {
        result.create(window, WindowDebugServerOptions {.port = port});
    }
    catch (const std::exception&)
    {
        // Most likely the port is taken — another debug-enabled app (or a
        // second instance). An ephemeral port keeps every instance
        // reachable; port() / the LOG line say where.
        LOG("WindowDebugServer: port " + std::to_string(port)
            + " unavailable, falling back to an ephemeral port");
        result.create(window, WindowDebugServerOptions {.port = 0});
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

WindowDebugServer* currentServer()
{
    return gCurrentServer;
}

void attachExtensionOrDefer(OwningPointer<ServerExtension> extension)
{
    if (gCurrentServer)
        gCurrentServer->addExtension(std::move(extension));
    else
        pendingExtensions().add(std::move(extension));
}

bool installWindowAutoAttach()
{
    Graphics::Detail::windowDebugAttachFactory() = [](Graphics::Window& window)
        -> OwningPointer<Graphics::Detail::DebugAttachment>
    {
        // One endpoint per app: only the primary window hosts the server.
        if (gCurrentServer)
            return {};

        auto port = resolveDebugPort(getEnv("EACP_DEBUG_PORT"));
        if (!port)
            return {};

        try
        {
            auto server = makeServer(window, *port);
            gCurrentServer = server.get();

            // Adopt anything a higher layer handed in before now.
            for (auto& extension: pendingExtensions())
                gCurrentServer->addExtension(std::move(extension));
            pendingExtensions().clear();

            auto attachment = OwningPointer<Graphics::Detail::DebugAttachment> {};
            attachment.create<ServerAttachment>(std::move(server));
            return attachment;
        }
        catch (const std::exception& e)
        {
            // A broken debug affordance must never take the app down.
            LOG(std::string {"WindowDebugServer: failed to start: "} + e.what());
            return {};
        }
    };

    return true;
}

} // namespace eacp::Graphics::Remote
