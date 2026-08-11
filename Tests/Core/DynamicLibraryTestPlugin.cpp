// Fixture for DynamicLibraryTests: statics only re-run on a genuine load, so
// bumping EACP_TEST_PLUGIN_LOADS here observes real unmaps.
#include <eacp/Core/Plugins/PluginExport.h>
#include <eacp/Core/Utils/Environment.h>

#include <string>

namespace
{
int currentLoadCount()
{
    auto value = eacp::getEnvValue("EACP_TEST_PLUGIN_LOADS");
    return value.empty() ? 0 : std::stoi(value);
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
