#pragma once

#include "Menu.h"

#include <eacp/Core/Utils/WinInclude.h>

namespace eacp::Graphics::detail
{
// A Win32 menu is owned by an HWND and reports through that window's message
// loop, so Menu-Windows.cpp owns the menu and Window-Windows.cpp's WndProc
// routes WM_COMMAND and WM_INITMENUPOPUP into it.

// Drops whatever was there before. Safe to call repeatedly.
void installWin32MenuBar(HWND hwnd, const MenuBar& bar);

// True when the id belonged to this window's menu bar and the action ran; on
// false the WndProc should fall through to DefWindowProcW.
bool handleWin32MenuCommand(HWND hwnd, unsigned id);

// WM_INITMENUPOPUP: greys each item from its own predicate just before the
// popup is drawn, so the greying follows live state.
void updateWin32MenuEnabledState(HWND hwnd);

// Call from the window's teardown, so a later window reusing the HWND address
// cannot inherit this menu.
void removeWin32MenuBar(HWND hwnd);
} // namespace eacp::Graphics::detail
