#pragma once

#include "Keyboard.h"

#include <string_view>

// Both directions of the web's key table, from KeyboardEvent.code (the
// physical key, "KeyA", "ShiftLeft"), and the keyboard state the window's key
// events leave behind for the polled queries. The native unit of
// Window::isKeyPressed is the KeyCode itself, as on Android.

namespace eacp::Graphics
{
// KeyCode::Unknown for a code outside the table.
uint16_t webKeyCodeFromCode(std::string_view code);

// Empty for a KeyCode with no DOM code behind it.
std::string_view webCodeFromKeyCode(uint16_t keyCode);

// Shift 1, Control 2, Alt 4, Meta 8, which is how Window-Web.js packs them.
ModifierKeys webModifiersFromMask(int mask);

// The KeyEvent a DOM keydown or keyup makes: `key` is KeyboardEvent.key,
// which is the text for a printable key and a name ("Enter") otherwise.
KeyEvent webKeyEvent(bool down,
                     std::string_view code,
                     std::string_view key,
                     int modifierMask,
                     bool isRepeat,
                     double timestampMs);

void webKeyboardEvent(const KeyEvent& event);

// The page lost focus: no key up will come for what is held.
void webKeyboardReset();
} // namespace eacp::Graphics
