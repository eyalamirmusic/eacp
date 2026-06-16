#pragma once

#include <eacp/Core/Utils/WinInclude.h>

namespace eacp::Graphics
{
// Opt the process into the system's dark control/menu theming so the classic
// menus drawn by TrackPopupMenu follow the system appearance. Idempotent and
// cheap after the first call; invoke from any window or tray setup path.
void ensureDarkModeAppInitialised();

// Reload the cached menu theme so already-built popup menus pick up a live
// light/dark switch. Pair with isThemeChangeMessage() on WM_SETTINGCHANGE.
void refreshMenuTheme();

// Apply the dark or light caption colour to a top-level window's title bar.
void applyTitleBarTheme(HWND hwnd, bool dark);

// True when a WM_SETTINGCHANGE notifies that the system colours/theme changed
// (lParam points at the area name "ImmersiveColorSet").
bool isThemeChangeMessage(LPARAM lParam);
} // namespace eacp::Graphics
