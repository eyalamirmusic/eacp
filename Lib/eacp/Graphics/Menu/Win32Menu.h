#pragma once

#include "Menu.h"

#include <eacp/Core/Utils/WinInclude.h>

namespace eacp::Graphics::detail
{
// The seam between a window and the menu bar attached to it.
//
// A Win32 menu is owned by an HWND and reports back through that window's
// message loop, so the two halves cannot live in one file: Menu-Windows.cpp
// owns the menu and its command table, and Window-Windows.cpp owns the WndProc
// that has to route WM_COMMAND and WM_INITMENUPOPUP into it.

// Builds the native menu, attaches it to `hwnd` and drops whatever was there
// before. Safe to call repeatedly on the same window.
void installWin32MenuBar(HWND hwnd, const MenuBar& bar);

// WM_COMMAND. True when the id belonged to this window's menu bar and the
// action was run, false for every other source of WM_COMMAND — the WndProc
// should fall through to DefWindowProcW on false.
bool handleWin32MenuCommand(HWND hwnd, unsigned id);

// WM_INITMENUPOPUP. Greys each item from its own predicate just before the
// popup is drawn, which is where Win32 asks the question that
// NSMenuValidation's validateMenuItem: answers on macOS. Doing it here rather
// than at build time is what lets an app install its bar once and still have
// the greying follow live state.
void updateWin32MenuEnabledState(HWND hwnd);

// Frees the menu and forgets the window. Called from the window's teardown, so
// a destroyed HWND cannot be matched by a later window that reuses its address.
void removeWin32MenuBar(HWND hwnd);
} // namespace eacp::Graphics::detail
