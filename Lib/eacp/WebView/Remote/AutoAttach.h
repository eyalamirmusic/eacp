#pragma once

#include <optional>
#include <string>

namespace eacp::WebView::Remote
{

// Where the debug server listens when EACP_DEBUG_PORT is unset. Taken
// by another instance -> falls back to an ephemeral port (logged).
inline constexpr auto defaultDebugPort = 9696;

// Maps the EACP_DEBUG_PORT environment value to a port: unset ->
// defaultDebugPort, "off" -> nullopt (disabled), a number -> that
// number (0 = ephemeral). Exposed for tests.
std::optional<int> resolveDebugPort(const std::optional<std::string>& value);

// Installs the WebViewBridge hook (see WebView/DebugAttach.h) so every
// bridge constructed afterwards in a non-headless app gets a
// DebugServer attached. Don't call this by hand — apps get it through
// AutoAttachRegister.cpp, which eacp_add_webview_app compiles into the
// app binary when EACP_DEBUG_SERVER allows it (AUTO -> Debug builds
// only, so production binaries never contain the server). Returns true
// for static-initializer use.
bool installAutoAttach();

} // namespace eacp::WebView::Remote
