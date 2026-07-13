// Fixture for DynamicLibraryTests: a minimal plugin whose static initializer
// bumps EACP_TEST_PLUGIN_LOADS in the process environment. Statics only
// re-run on a genuine load, so the counter observes real unmaps — a resident
// image reopened does not re-initialize.
#include <eacp/Core/Plugins/PluginExport.h>

#include <cstdlib>
#include <string>

namespace
{
int bumpLoadCount()
{
    auto* existing = std::getenv("EACP_TEST_PLUGIN_LOADS");
    auto next = std::to_string((existing != nullptr ? std::atoi(existing) : 0) + 1);

#ifdef _WIN32
    _putenv_s("EACP_TEST_PLUGIN_LOADS", next.c_str());
#else
    setenv("EACP_TEST_PLUGIN_LOADS", next.c_str(), 1);
#endif

    return 0;
}

[[maybe_unused]] const auto loadStamp = bumpLoadCount();
} // namespace

EACP_PLUGIN_EXPORT int eacpTestPluginAdd(int a, int b)
{
    return a + b;
}
