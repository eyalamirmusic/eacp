#pragma once

namespace eacp::Graphics
{
// True when the OS is currently using a dark interface theme. Reflects the
// live system setting at call time (Windows: the Personalize
// AppsUseLightTheme preference; macOS: the application's effective
// appearance; iOS: the main screen's user-interface style).
bool isSystemDarkMode();
} // namespace eacp::Graphics
