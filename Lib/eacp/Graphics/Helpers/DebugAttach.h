#pragma once

#include <eacp/Core/Utils/Containers.h>
#include <eacp/Core/Utils/Singleton.h>

#include <functional>

namespace eacp::Graphics
{

class Window;

namespace Detail
{

// Type-erased ownership handle for whatever a debug transport attaches
// to a Window (the embedded MCP capture/debug server, in practice).
// Lives in the core graphics lib — not in any transport — so Window can
// hold one without depending on eacp-graphics-remote or eacp-webview.
struct DebugAttachment
{
    virtual ~DebugAttachment() = default;
};

using WindowDebugAttachFactory =
    std::function<OwningPointer<DebugAttachment>(Window&)>;

// Empty until a debug transport installs itself — the window auto-attach
// register TU does, when an app is built with the debug server enabled.
// Every Window constructed afterwards in a non-headless app consults
// this and keeps the returned attachment alive for its own lifetime
// (see Window::attachDebugServer).
inline WindowDebugAttachFactory& windowDebugAttachFactory()
{
    return Singleton::get<WindowDebugAttachFactory>();
}

} // namespace Detail

} // namespace eacp::Graphics
