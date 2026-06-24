#pragma once

#include <eacp/Core/Utils/Containers.h>

#include <optional>
#include <string>

namespace eacp::Graphics::Remote
{

class WindowDebugServer;
struct ServerExtension;

// Where the debug server listens when EACP_DEBUG_PORT is unset. Taken by
// another instance -> falls back to an ephemeral port (logged).
inline constexpr auto defaultDebugPort = 9696;

// Maps the EACP_DEBUG_PORT environment value to a port: unset ->
// defaultDebugPort, "off" -> nullopt (disabled), a number -> that number
// (0 = ephemeral). Exposed for tests.
std::optional<int> resolveDebugPort(const std::optional<std::string>& value);

// Installs the Window debug-attach hook (see Graphics/Helpers/
// DebugAttach.h) so the first window of a non-headless app gets a
// WindowDebugServer. Don't call this by hand — apps get it through
// WindowAutoAttachRegister.cpp, compiled into the app binary when the
// build enables the debug server. Returns true for static-initializer
// use.
bool installWindowAutoAttach();

// The WindowDebugServer bound to the app's primary window, or null if
// none has been created. Lets a higher layer reach the one server.
WindowDebugServer* currentServer();

// Hand an extension to the current server, or stash it to be adopted the
// moment the server is created — so a higher layer (the WebView DOM
// tools) can enrich the server regardless of whether the Window or the
// enriching object was constructed first.
void attachExtensionOrDefer(OwningPointer<ServerExtension> extension);

} // namespace eacp::Graphics::Remote
