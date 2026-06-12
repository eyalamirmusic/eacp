// Compiled into the app binary (not into eacp-webview-remote) by
// eacp_add_webview_app when EACP_DEBUG_SERVER enables the debug server
// for the current build type — a TU handed directly to the linker is
// never dead-stripped the way an unreferenced archive member is, so
// this initializer reliably runs before main().
#include "AutoAttach.h"

namespace
{
[[maybe_unused]] const auto installed = eacp::WebView::Remote::installAutoAttach();
}
