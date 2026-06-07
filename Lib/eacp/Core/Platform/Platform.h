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

std::string_view name();

} // namespace eacp::Platform
