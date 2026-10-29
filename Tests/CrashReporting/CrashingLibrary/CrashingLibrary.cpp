#include <eacp/Core/Plugins/PluginExport.h>

#include <stdexcept>

// A library CrashReportingTests loads at runtime to crash inside an image the
// executable did not link.

#if defined(_MSC_VER)
#pragma optimize("", off)
#endif

EACP_PLUGIN_EXPORT void crashingLibraryWriteThroughNullPointer()
{
    auto* volatile target = static_cast<int*>(nullptr);
    *target = 1;
}

EACP_PLUGIN_EXPORT void crashingLibraryThrowUncaughtException()
{
    throw std::runtime_error {"the crashing library threw this on purpose"};
}
