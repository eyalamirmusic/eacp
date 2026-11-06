#pragma once

/** @file Internal: macOS only: converts NSEvents into KeyEvent and ModifierKeys. */

#include "Keyboard.h"

namespace eacp::Graphics
{
ModifierKeys modifierKeysFromEvent(NSEvent* event);
KeyEvent keyEventFrom(NSEvent* event, KeyEventType type);
}