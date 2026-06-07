#include "Platform.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace eacp::Platform
{

OS current()
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

bool isMac()
{
    return current() == OS::MacOS;
}

bool isIOS()
{
    return current() == OS::iOS;
}

bool isApple()
{
    return isMac() || isIOS();
}

bool isWindows()
{
    return current() == OS::Windows;
}

bool isLinux()
{
    return current() == OS::Linux;
}

bool isPosix()
{
    return isApple() || isLinux();
}

std::string_view name()
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

} // namespace eacp::Platform
