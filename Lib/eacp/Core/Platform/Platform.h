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
    Linux,
    Android
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
#elif defined(__ANDROID__)
    return OS::Android;
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

constexpr bool isAndroid()
{
    return current() == OS::Android;
}

// Desktop Linux only. Android shares its kernel, its Vulkan backend and its
// GLSL, and nothing else — not its fonts, its files or its process model — so
// code that means the backend asks isLinuxFamily().
constexpr bool isLinux()
{
    return current() == OS::Linux;
}

constexpr bool isLinuxFamily() // Linux || Android
{
    return isLinux() || isAndroid();
}

constexpr bool isPosix() // Apple || Linux family
{
    return isApple() || isLinuxFamily();
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
        case OS::Android:
            return "Android";
    }
    return "Unknown";
}

// The CPU architecture this binary was built for: "arm64" or "x64".
constexpr std::string_view architectureName()
{
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#else
    return "x64";
#endif
}

// The role of this eacp copy. isStandalone when it is the application: it is
// compiled into the process executable, or into a library the executable
// claims (Core/Plugins/ClaimCopy.h). isDLL when it is a guest in a process
// someone else owns — a plugin. Decided at runtime from the loaded images
// (Plugins::isDynamicLibrary), so every statically linked eacp copy in a
// process answers for itself, and so these cannot be constexpr. Apps::run<T>
// uses it to decide loop ownership: a guest app is scheduled onto the host's
// loop instead of running its own. An Android app is a library NativeActivity
// loads and is standalone all the same.
bool isStandalone();
bool isDLL();

// The running app's name, company and version, read from the AppInfo.json
// that eacp_set_gui_subsystem embeds via ResEmbed. Empty when this binary has
// no embedded AppInfo (e.g. a console app that never called the CMake helper);
// the company is also empty when the target set none.
std::string_view getAppName();
std::string_view getCompanyName();
std::string_view getAppVersion();

} // namespace eacp::Platform
