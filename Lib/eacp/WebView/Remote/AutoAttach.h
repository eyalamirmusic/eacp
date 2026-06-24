#pragma once

namespace eacp::WebView::Remote
{

// Installs the WebViewBridge hook (see WebView/DebugAttach.h) so every
// bridge constructed afterwards in a non-headless app contributes its
// DOM-driving tools to the app's WindowDebugServer (the window-level
// capture server). Don't call this by hand — apps get it through
// AutoAttachRegister.cpp, which the debug-server cmake helper compiles
// into the app binary when EACP_DEBUG_SERVER allows it. Returns true for
// static-initializer use.
//
// The listen port and the capture tools live with the window server now
// (Graphics/Remote/WindowAutoAttach.h); this hook only adds DOM tools.
bool installAutoAttach();

} // namespace eacp::WebView::Remote
