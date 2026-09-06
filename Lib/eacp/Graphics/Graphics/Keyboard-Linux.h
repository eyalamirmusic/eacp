#pragma once

#include "Keyboard.h"

// The two directions of the Linux key table, shared by the file that owns it
// (Keyboard-Linux.cpp) and the file that builds KeyEvents with it
// (WaylandInput-Linux.cpp).
//
// The native unit on Linux is the evdev keycode - what the kernel's input
// layer numbers a physical key, what wl_keyboard.key carries, and what xkb
// calls a keycode once 8 has been added to it. Positional like the macOS
// virtual key codes the framework's KeyCode values are, so a shortcut bound to
// a physical key means the same thing on both, which is the whole point of
// keying shortcuts on KeyCode rather than on the character a key produced.

namespace eacp::Graphics
{
// KeyCode::Unknown for a key outside the table - a media key, an extra mouse
// button's keyboard alias, anything a layout invents.
uint16_t waylandKeyCodeFromEvdev(uint32_t evdevCode);

// Zero for a KeyCode with no evdev key behind it, which today is only
// KeyCode::Unknown itself.
uint32_t waylandEvdevFromKeyCode(uint16_t keyCode);
} // namespace eacp::Graphics
