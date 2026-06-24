#pragma once

#include <eacp/Core/Utils/Containers.h>
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

// Type-erased ownership handle for whatever a debug transport attaches
// to a WebView + bridge pair (the remote MCP debug server, in
// practice). Lives here so the core WebView lib stays free of any
// dependency on eacp-webview-remote.
struct DebugAttachment
{
    virtual ~DebugAttachment() = default;
};

using DebugAttachFactory =
    std::function<OwningPointer<DebugAttachment>(WebView&, Miro::Bridge&)>;

// Empty until a debug transport installs itself — AutoAttachRegister.cpp
// does, when eacp_add_webview_app compiles the debug server into the
// binary (EACP_DEBUG_SERVER: AUTO -> Debug builds only). WebViewBridge
// consults this at construction and keeps the returned attachment
// alive for its own lifetime.
inline DebugAttachFactory& debugAttachFactory()
{
    return Singleton::get<DebugAttachFactory>();
}

} // namespace Detail

} // namespace eacp::Graphics
