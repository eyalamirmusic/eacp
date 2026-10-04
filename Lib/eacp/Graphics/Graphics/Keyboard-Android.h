#pragma once

#include "Keyboard.h"

// Both directions of the Android key table, the text a key types, and the
// keyboard state the window's key events leave behind for the polled queries.

namespace eacp::Graphics
{
// KeyCode::Unknown for a key outside the table.
uint16_t androidKeyCodeFromNative(int32_t androidCode);

// AKEYCODE_UNKNOWN for a KeyCode with no Android key behind it.
int32_t androidNativeFromKeyCode(uint16_t keyCode);

ModifierKeys androidModifiersFromMeta(int32_t metaState);

// What the key types under that meta state, from the virtual keyboard's key
// map; empty for a key that types nothing or a dead key.
std::string androidKeyText(int32_t androidCode, int32_t metaState);

void androidKeyboardEvent(const KeyEvent& event);

// The window lost focus: no key up will come for what is held.
void androidKeyboardReset();
} // namespace eacp::Graphics
