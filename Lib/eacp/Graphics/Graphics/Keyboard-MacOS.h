#pragma once

#include "Keyboard.h"

namespace eacp::Graphics
{
ModifierKeys modifierKeysFromEvent(NSEvent* event);
KeyEvent keyEventFrom(NSEvent* event, KeyEventType type);
}