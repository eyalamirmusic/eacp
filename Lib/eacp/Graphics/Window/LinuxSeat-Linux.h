#pragma once

#include "../Graphics/Keyboard.h"
#include "../Primitives/Primitives.h"

#include <string>

// What the seat can be asked with no window in hand: the polled keyboard
// state Keyboard's window-less queries read, and the pointer's window,
// position and cursor a View asks about. Wayland's wl_seat and X11's core
// devices both answer it, so nothing above this has to know which is under it.

namespace eacp::Graphics
{
struct LinuxWindowSurface;

class LinuxSeat
{
public:
    virtual ~LinuxSeat() = default;

    // Polled keyboard state, in the native (evdev) unit.
    virtual bool isKeyPressed(uint32_t evdevCode) const = 0;
    virtual Vector<uint32_t> getPressedCodes() const = 0;
    virtual ModifierKeys getModifiers() const = 0;

    // What the key would type on the current layout with no modifiers
    // applied. Not gated on focus: the keymap outlives the focus that
    // delivered it.
    virtual std::string characterForCode(uint32_t evdevCode) const = 0;

    // Null while the keys are going somewhere that is not ours.
    virtual LinuxWindowSurface* getKeyboardFocus() const = 0;

    bool hasKeyboardFocus() const { return getKeyboardFocus() != nullptr; }

    // Null and {} until the pointer has entered a window of ours.
    virtual LinuxWindowSurface* getPointerWindow() const = 0;
    virtual Point getPointerPosition() const = 0;

    // Re-reads the shape under the pointer and applies it.
    virtual void refreshCursor() = 0;
};
} // namespace eacp::Graphics
