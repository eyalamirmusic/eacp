// Fixture for DynamicLibraryTests: a minimal plugin whose static initializer
// bumps EACP_TEST_PLUGIN_LOADS in the process environment. Statics only
// re-run on a genuine load, so the counter observes real unmaps — a resident
// image reopened does not re-initialize.
#include <eacp/Core/Plugins/PluginExport.h>
#include <eacp/Core/Utils/Environment.h>
#include <eacp/Core/Utils/Strings.h>

#include <string>

namespace
{
int currentLoadCount()
{
    return eacp::Strings::parseIntOr(eacp::getEnvValue("EACP_TEST_PLUGIN_LOADS"));
}

int bumpLoadCount()
{
    eacp::setEnv("EACP_TEST_PLUGIN_LOADS", std::to_string(currentLoadCount() + 1));

    return 0;
}

[[maybe_unused]] const auto loadStamp = bumpLoadCount();
} // namespace

EACP_PLUGIN_EXPORT int eacpTestPluginAdd(int a, int b)
{
    return a + b;
}
