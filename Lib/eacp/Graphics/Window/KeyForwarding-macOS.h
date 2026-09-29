#pragma once

#include "KeyForwarding.h"

@class NSEvent;

namespace eacp::Graphics
{
NativeKeyEvent nativeKeyEventFrom(NSEvent* event);
} // namespace eacp::Graphics
