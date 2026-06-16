#include <eacp/Core/Utils/WinInclude.h>

#include "DarkMode-Windows.h"
#include "SystemAppearance.h"

#include <dwmapi.h>
#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "Advapi32.lib")

#include <mutex>

namespace eacp::Graphics
{
namespace
{
// The dark-mode app-mode controls are exported from uxtheme.dll only by
// ordinal (no public header). The ordinals below are stable across Windows 10
// 1809+ and Windows 11; resolve them once and degrade to no-ops if missing.
enum class PreferredAppMode
{
    Default = 0,
    AllowDark = 1,
    ForceDark = 2,
    ForceLight = 3,
};

using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode);
using FlushMenuThemesFn = void(WINAPI*)();

struct UxThemeDarkMode
{
    UxThemeDarkMode()
    {
        module =
            LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module)
            return;

        setPreferredAppMode = reinterpret_cast<SetPreferredAppModeFn>(
            GetProcAddress(module, MAKEINTRESOURCEA(135)));
        flushMenuThemes = reinterpret_cast<FlushMenuThemesFn>(
            GetProcAddress(module, MAKEINTRESOURCEA(136)));
    }

    HMODULE module = nullptr;
    SetPreferredAppModeFn setPreferredAppMode = nullptr;
    FlushMenuThemesFn flushMenuThemes = nullptr;
};

const UxThemeDarkMode& uxTheme()
{
    static const UxThemeDarkMode instance;
    return instance;
}

// DWMWA_USE_IMMERSIVE_DARK_MODE moved from 19 to 20 in Windows 10 20H1. Try
// the modern id first and fall back so 1809-1909 still get the dark caption.
constexpr DWORD immersiveDarkModeAttribute = 20;
constexpr DWORD immersiveDarkModeAttributeLegacy = 19;
} // namespace

bool isSystemDarkMode()
{
    DWORD appsUseLightTheme = 1; // assume light if the key is absent
    DWORD size = sizeof(appsUseLightTheme);
    RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD,
        nullptr,
        &appsUseLightTheme,
        &size);
    return appsUseLightTheme == 0;
}

void ensureDarkModeAppInitialised()
{
    static std::once_flag once;
    std::call_once(once,
                   []
                   {
                       if (auto setMode = uxTheme().setPreferredAppMode)
                           setMode(PreferredAppMode::AllowDark);

                       refreshMenuTheme();
                   });
}

void refreshMenuTheme()
{
    if (auto flush = uxTheme().flushMenuThemes)
        flush();
}

void applyTitleBarTheme(HWND hwnd, bool dark)
{
    if (!hwnd)
        return;

    BOOL value = dark ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(
            hwnd, immersiveDarkModeAttribute, &value, sizeof(value))))
        DwmSetWindowAttribute(
            hwnd, immersiveDarkModeAttributeLegacy, &value, sizeof(value));
}

bool isThemeChangeMessage(LPARAM lParam)
{
    auto* area = reinterpret_cast<const wchar_t*>(lParam);
    return area != nullptr && lstrcmpiW(area, L"ImmersiveColorSet") == 0;
}

} // namespace eacp::Graphics
