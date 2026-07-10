#include <eacp/Core/Core.h>
#include <eacp/Core/Plugins/DynamicLibrary.h>

int main()
{
    auto library = eacp::Plugins::DynamicLibrary(DEMO_PLUGIN_PATH);

    if (!library.isOpen())
    {
        eacp::LOG("Host: failed to load ", DEMO_PLUGIN_PATH);
        return 1;
    }

    for (auto& name: library.getFunctionNames())
        eacp::LOG("Host: plugin exports '", name, "'");

    if (auto getName = library.findFunction<const char* (*) ()>("demo_get_name"))
        eacp::LOG("Host: loaded '", getName(), "'");

    if (auto ping = library.findFunction<void (*)()>("demo_ping"))
        ping();

    library.close();
    eacp::LOG("Host: plugin unloaded");

    return 0;
}
