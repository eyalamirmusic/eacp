#pragma once

/** @file Whether the operating system is in dark mode. */

namespace eacp::Graphics
{
// True when the OS is currently using a dark interface theme. Reflects the
// live system setting at call time (Windows: the Personalize
// AppsUseLightTheme preference; macOS: the application's effective
// appearance; iOS: the main screen's user-interface style; Android: the
// activity configuration's night mode; Linux: always false).
bool isSystemDarkMode();
} // namespace eacp::Graphics
