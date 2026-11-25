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
    Android,
    Web
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
#elif defined(__EMSCRIPTEN__)
    return OS::Web;
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

// WebAssembly through Emscripten, in a browser or under node.
constexpr bool isWeb()
{
    return current() == OS::Web;
}

// Whether a Thread can run a body. The web build is single-threaded: a page is
// a guest on the browser's one loop, so Thread::start refuses and a ThreadPool
// runs its work on the main thread. EACP_HAS_THREADS in CMake.
constexpr bool hasThreads()
{
    return !isWeb();
}

// Apple || Linux family || Web. The web counts because Emscripten's musl is the
// POSIX API its -Posix sources compile against; it has no processes, signals
// or dynamic libraries behind that API.
constexpr bool isPosix()
{
    return isApple() || isLinuxFamily() || isWeb();
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
        case OS::Web:
            return "Web";
    }
    return "Unknown";
}

// The CPU architecture this binary was built for: "arm64", "x64" or "wasm32".
constexpr std::string_view architectureName()
{
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__wasm32__)
    return "wasm32";
#else
    return "x64";
#endif
}

// Role of this eacp copy: isStandalone when it is the application (compiled
// into the process executable, or into a library the executable claims with
// Plugins/ClaimCopy.h), isDLL when it is a guest in someone else's process (a
// plugin). Resolved at runtime from the loaded images
// (Plugins::isDynamicLibrary), so every statically linked eacp copy in a
// process answers for itself, and so these cannot be constexpr. Apps::run<T>
// uses it to decide loop ownership: a DLL app is scheduled onto the host's
// loop instead of running its own.
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
