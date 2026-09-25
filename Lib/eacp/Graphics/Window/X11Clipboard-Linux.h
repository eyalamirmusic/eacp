#pragma once

#include "LinuxWindowSystem-Linux.h"
#include "X11Connection-Linux.h"

#include <string>

// The CLIPBOARD selection, behind the Clipboard::Backend hooks. Owned by
// X11Connection, which offers the backend to LinuxWindowSystem as the
// connection comes up and takes it away again on the way down, so a copy that
// prefers Wayland - or reaches no server at all - keeps Core's empty answers.
//
// X11 has no clipboard of its own, any more than Wayland has: the selection
// belongs to whichever client last claimed it, and a paste is a property the
// owner writes onto the requestor's window. What differs from the Wayland twin
// is who may claim it. There, set_selection needs the serial of an input event
// on a surface of ours, so a copy needs a focused window; here the owner is a
// 1x1 window that is created on first use and never mapped, so a plugin copy
// with no toplevel and no keyboard focus can still put text on the clipboard.
//
// A read blocks the message thread while the connection is pumped by hand
// (X11Connection::dispatchUntil), so everything arriving beside the answer is
// dispatched exactly as the loop source would have dispatched it and no window
// loses an event to a paste. Reading an INCR transfer is implemented, which is
// how every toolkit sends anything large; sending one is not, so a selection
// bigger than the server's maximum request is refused rather than truncated.

namespace eacp::Graphics
{
class X11Clipboard
{
public:
    explicit X11Clipboard(X11Connection& connectionToUse);
    ~X11Clipboard();

    X11Clipboard(const X11Clipboard&) = delete;
    X11Clipboard& operator=(const X11Clipboard&) = delete;

    bool copyText(std::string_view text);
    bool copyFiles(const Vector<std::string>& paths);
    std::string getText();
    bool hasText();

    // True when the event was this object's: a request for the selection we
    // own, the answer to one we asked for, or the loss of ownership. All of
    // them name the clipboard's own window, which is nobody else's, so a
    // toplevel never loses an event here.
    bool handleEvent(const xcb_generic_event_t& event);

    // The server is gone: the store goes with it, and the hooks go back to the
    // answers a machine with no clipboard gives.
    void connectionLost();

private:
    enum class Content
    {
        None,
        Text,
        Files
    };

    // A hook is running whenever the connection is pumped from inside one, and
    // the loss may arrive there: clearing the backend then would destroy the
    // std::function whose body is on the stack, so the clear waits.
    struct Call;

    bool ensureWindow();

    xcb_connection_t* xcb() const { return connection.getConnection(); }
    const X11Atoms& atoms() const { return connection.getAtoms(); }

    xcb_window_t selectionOwner();
    bool takeSelection(std::string data, Content kind);

    void serveRequest(const xcb_selection_request_event_t& request);
    bool writeRequested(const xcb_selection_request_event_t& request,
                        xcb_atom_t property);
    bool writeTargets(xcb_window_t requestor, xcb_atom_t property);
    bool writeData(xcb_window_t requestor, xcb_atom_t property, xcb_atom_t type);
    void answerRequest(const xcb_selection_request_event_t& request,
                       xcb_atom_t property);

    Vector<xcb_atom_t> offeredTargets() const;
    bool isTextTarget(xcb_atom_t target) const;

    // XCB_ATOM_NONE when the owner offers nothing this can read as text.
    xcb_atom_t pickTextTarget();
    Vector<xcb_atom_t> availableTargets();

    std::string convertSelection(xcb_atom_t target);
    std::string readProperty(xcb_atom_t& type);
    std::string readIncrementally();

    X11Connection& connection;

    xcb_window_t window = XCB_NONE;

    std::string ownedData;
    Content ownedContent = Content::None;

    // Set by handleEvent, read by the waits in convertSelection: an answer may
    // already be in the queue by the time one starts waiting for it.
    bool notifyArrived = false;
    bool chunkArrived = false;
    xcb_atom_t notifyProperty = XCB_ATOM_NONE;

    int callsInFlight = 0;
    bool clearWhenIdle = false;
};
} // namespace eacp::Graphics
