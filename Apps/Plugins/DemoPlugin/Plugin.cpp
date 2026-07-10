#include <eacp/Core/Plugins/PluginExport.h>
#include <eacp/Core/Utils/Logging.h>

EACP_PLUGIN_EXPORT const char* demo_get_name()
{
    return "Demo Plugin 1.0";
}

EACP_PLUGIN_EXPORT void demo_ping()
{
    eacp::LOG("DemoPlugin: ping handled by the plugin's own eacp copy");
}
