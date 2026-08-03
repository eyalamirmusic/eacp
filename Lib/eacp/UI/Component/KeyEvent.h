#pragma once

#include "../Common.h"

namespace eacp::UI
{
// A key event as the component receiving it sees it, which is exactly as the
// native view saw it.
//
// An alias rather than a type of its own, unlike MouseEvent. What made that one
// worth converting is that a position means nothing until it is in the receiving
// component's space; a key has no position, and every field it does carry --
// the code, the characters, the modifiers, whether it repeated -- means the same
// thing wherever in the tree it lands. A parallel struct here would be a copy
// per keystroke and a second place for KeyCode to be spelled.
using KeyEvent = eacp::Graphics::KeyEvent;

namespace KeyCode = eacp::Graphics::KeyCode;

using ModifierKeys = eacp::Graphics::ModifierKeys;
} // namespace eacp::UI
