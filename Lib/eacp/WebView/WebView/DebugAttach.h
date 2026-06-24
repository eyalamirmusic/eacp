#pragma once

#include <eacp/Core/Utils/Singleton.h>

#include <functional>

namespace Miro
{
class Bridge;
}

namespace eacp::Graphics
{

class WebView;

namespace Detail
{

// Notifies a debug transport of each (WebView, bridge) pair so it can add
// DOM-driving tools to the app's window server. Empty until a transport
// installs itself (WebView/Remote/AutoAttachRegister.cpp). The window
// server owns the tools, so this returns nothing — unlike the window-level
// hook in Graphics/Helpers/DebugAttach.h, which hands the Window an
// attachment to own.
using WebViewDebugHook = std::function<void(WebView&, Miro::Bridge&)>;

inline WebViewDebugHook& webViewDebugHook()
{
    return Singleton::get<WebViewDebugHook>();
}

} // namespace Detail

} // namespace eacp::Graphics
