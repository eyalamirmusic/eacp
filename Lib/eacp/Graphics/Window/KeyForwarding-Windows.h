#pragma once

/** @file Internal: Windows only. Builds a NativeKeyEvent from a Win32 key message. */

#include <eacp/Core/Utils/WinInclude.h>

#include "KeyForwarding.h"

namespace eacp::Graphics
{
bool isPlainKeyMessage(UINT message);

NativeKeyEvent
    nativeKeyEventFrom(UINT message, WPARAM wParam, LPARAM lParam, DWORD time);
} // namespace eacp::Graphics
