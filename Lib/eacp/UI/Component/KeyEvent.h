#pragma once

#include "../Common.h"

namespace eacp::UI
{
// Passed through unconverted, unlike MouseEvent: no field is space-dependent.
using KeyEvent = eacp::Graphics::KeyEvent;

namespace KeyCode = eacp::Graphics::KeyCode;

using ModifierKeys = eacp::Graphics::ModifierKeys;
} // namespace eacp::UI
