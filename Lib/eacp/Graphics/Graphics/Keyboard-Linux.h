#pragma once

#include "Keyboard.h"

// Both directions of the Linux key table. An evdev keycode is what a Wayland
// seat carries, what an X11 keycode is once 8 has been subtracted, and an xkb
// keycode once 8 has been added.

namespace eacp::Graphics
{
// KeyCode::Unknown for a key outside the table.
uint16_t linuxKeyCodeFromEvdev(uint32_t evdevCode);

// Zero for a KeyCode with no evdev key behind it.
uint32_t linuxEvdevFromKeyCode(uint16_t keyCode);
} // namespace eacp::Graphics
