#pragma once

#include "Keyboard.h"

@class UIPress;

namespace eacp::Graphics
{
// KeyCode::Unknown for a key outside the table of USB HID usages a UIKey reports.
KeyEvent iosKeyEventFromPress(UIPress* press, KeyEventType type);

// Keeps the keys and modifiers a hardware press leaves held, for the polled queries.
void iosKeyboardEvent(const KeyEvent& event);
} // namespace eacp::Graphics
