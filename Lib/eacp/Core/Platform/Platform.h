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

// The OS this binary was built for. Platform.cpp holds the library's only
// compile-time platform check; everything else queries it at runtime.
OS current();

bool isMac(); // macOS desktop only
bool isIOS();
bool isApple(); // macOS || iOS
bool isWindows();
bool isLinux();
bool isPosix(); // Apple || Linux

// Linkage of this eacp copy, resolved from its own image, so every statically
// linked copy in a process answers for itself.
bool isStandalone();
bool isDLL();

std::string_view name();

// Read from the AppInfo.json eacp_set_gui_subsystem embeds. Empty when this
// binary has none.
std::string_view getAppName();
std::string_view getAppVersion();

} // namespace eacp::Platform
