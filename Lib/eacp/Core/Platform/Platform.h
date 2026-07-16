#pragma once

#include <string_view>

namespace eacp::Platform
{

enum class OS
{
    MacOS,
    iOS,
    Windows,
    Linux
};

// The operating system this binary was built for. Resolved from the single
// compile-time platform check in Platform.cpp — the one place in the library
// that branches on platform macros. Everything else queries it at runtime, so
// shared code can pick behaviour without preprocessor switches.
OS current();

bool isMac(); // macOS desktop only
bool isIOS();
bool isApple(); // macOS || iOS
bool isWindows();
bool isLinux();
bool isPosix(); // Apple || Linux

// Linkage of this eacp copy: isStandalone when it is compiled into the
// process executable, isDLL when it lives in a dynamic library (a
// runtime-loaded plugin). Resolved from this copy's own image
// (Plugins::isDynamicLibrary), so every statically linked eacp copy in a
// process answers for itself. Apps::run<T> uses it to decide loop
// ownership: a DLL app is scheduled onto the host's loop instead of
// running its own.
bool isStandalone();
bool isDLL();

std::string_view name();

// The running app's name and version, read from the AppInfo.json that
// eacp_set_gui_subsystem embeds via ResEmbed. Empty when this binary has no
// embedded AppInfo (e.g. a console app that never called the CMake helper).
std::string_view getAppName();
std::string_view getAppVersion();

} // namespace eacp::Platform
