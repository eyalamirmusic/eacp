#pragma once

#include "Keyboard.h"

#include <string_view>

// The keyboard as the page's key events report it, kept by Window-Web.cpp and
// read by Keyboard-Web.cpp. Keys are KeyboardEvent.code: the physical key,
// whatever the layout makes of it.

namespace eacp::Graphics
{
// KeyCode::Unknown for a key outside the table.
uint16_t webKeyCodeFromCode(std::string_view code);

void webKeyChanged(uint16_t keyCode, bool pressed);
void webModifiersChanged(const ModifierKeys& modifiers);

// The page lost focus: its key-ups will go elsewhere.
void webReleaseAllKeys();
} // namespace eacp::Graphics
