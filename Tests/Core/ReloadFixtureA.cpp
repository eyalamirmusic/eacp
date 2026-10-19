#include <eacp/Core/Plugins/PluginExport.h>

EACP_PLUGIN_EXPORT int eacpReloadFixtureAnswer()
{
    return 1;
}

EACP_PLUGIN_EXPORT const char* eacpReloadFixtureName()
{
    return "A";
}
