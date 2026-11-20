#pragma once

#include "View.h"

@class NSEvent;
@class NSView;

namespace eacp::Graphics
{
// Whether `view` is one of the framework's OWN NSViews — the single class every
// eacp View is backed by (View-macOS.mm).
//
// It matters because that class ENDS the responder chain: its keyDown:/keyUp:
// hand the event to the C++ View and never call super, and View::keyDown has
// nowhere to pass on an event it did not want. So anything re-dispatching a key
// OUT of an eacp hierarchy — EmbedderKeyForwarder — has to walk past every one
// of them to reach the responder that embeds us.
bool isFrameworkNativeView(NSView* view);

// Every field of a MouseEvent that `event` carries for `type`, except pos and
// downPos, which depend on whose coordinates the caller wants.
MouseEvent mouseEventFromNSEvent(NSEvent* event, MouseEventType type);
} // namespace eacp::Graphics
