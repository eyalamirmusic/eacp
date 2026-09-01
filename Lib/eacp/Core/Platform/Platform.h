#pragma once

#include <string_view>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace eacp::Platform
{

enum class OS
{
    MacOS,
    iOS,
    Windows,
    Linux
};

// The operating system this binary was built for. The single compile-time
// platform check in the library — the one place that branches on platform
// macros. Everything else queries it instead, so shared code can pick
// behaviour without preprocessor switches. constexpr, so those queries fold
// away and `if constexpr` works on them.
constexpr OS current()
{
#if defined(_WIN32)
    return OS::Windows;
#elif defined(__APPLE__) && TARGET_OS_IPHONE
    return OS::iOS;
#elif defined(__APPLE__)
    return OS::MacOS;
#elif defined(__linux__)
    return OS::Linux;
#else
#error "eacp::Platform: unsupported target platform"
#endif
}

constexpr bool isMac() // macOS desktop only
{
    return current() == OS::MacOS;
}

constexpr bool isIOS()
{
    return current() == OS::iOS;
}

constexpr bool isApple() // macOS || iOS
{
    return isMac() || isIOS();
}

constexpr bool isWindows()
{
    return current() == OS::Windows;
}

constexpr bool isLinux()
{
    return current() == OS::Linux;
}

constexpr bool isPosix() // Apple || Linux
{
    return isApple() || isLinux();
}

constexpr std::string_view name()
{
    switch (current())
    {
        case OS::MacOS:
            return "macOS";
        case OS::iOS:
            return "iOS";
        case OS::Windows:
            return "Windows";
        case OS::Linux:
            return "Linux";
    }
    return "Unknown";
}

// Linkage of this eacp copy: isStandalone when it is compiled into the
// process executable, isDLL when it lives in a dynamic library (a
// runtime-loaded plugin). Resolved at runtime from this copy's own image
// (Plugins::isDynamicLibrary), so every statically linked eacp copy in a
// process answers for itself, and so these cannot be constexpr. Apps::run<T>
// uses it to decide loop ownership: a DLL app is scheduled onto the host's
// loop instead of running its own.
bool isStandalone();
bool isDLL();

// The running app's name and version, read from the AppInfo.json that
// eacp_set_gui_subsystem embeds via ResEmbed. Empty when this binary has no
// embedded AppInfo (e.g. a console app that never called the CMake helper).
std::string_view getAppName();
std::string_view getAppVersion();

} // namespace eacp::Platform
