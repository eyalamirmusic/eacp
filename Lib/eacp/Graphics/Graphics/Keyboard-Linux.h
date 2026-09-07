#pragma once

#include "Keyboard.h"

// Both directions of the Linux key table. An evdev keycode is what
// wl_keyboard.key carries, and an xkb keycode once 8 has been added to it.

namespace eacp::Graphics
{
// KeyCode::Unknown for a key outside the table.
uint16_t waylandKeyCodeFromEvdev(uint32_t evdevCode);

// Zero for a KeyCode with no evdev key behind it.
uint32_t waylandEvdevFromKeyCode(uint16_t keyCode);
} // namespace eacp::Graphics
