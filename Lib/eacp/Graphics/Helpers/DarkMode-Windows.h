#pragma once

#include <eacp/Core/Utils/WinInclude.h>

namespace eacp::Graphics
{
// Opts the process into dark control/menu theming, so TrackPopupMenu's classic
// menus follow the system appearance. Idempotent.
void ensureDarkModeAppInitialised();

// Reloads the cached menu theme; pair with isThemeChangeMessage().
void refreshMenuTheme();

void applyTitleBarTheme(HWND hwnd, bool dark);

// True when a WM_SETTINGCHANGE's lParam names "ImmersiveColorSet".
bool isThemeChangeMessage(LPARAM lParam);
} // namespace eacp::Graphics
