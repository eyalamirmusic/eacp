#pragma once

/** @file Internal: macOS only. Builds a NativeKeyEvent from an NSEvent. */

#include "KeyForwarding.h"

@class NSEvent;

namespace eacp::Graphics
{
NativeKeyEvent nativeKeyEventFrom(NSEvent* event);
} // namespace eacp::Graphics
