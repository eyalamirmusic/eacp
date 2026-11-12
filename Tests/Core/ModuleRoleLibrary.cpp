// Fixture for ModuleRoleTests: a library with its own eacp copy, reporting
// what that copy answers, its role also as taken in a static initialiser.
#include <eacp/Core/Platform/Platform.h>
#include <eacp/Core/Plugins/ModuleInfo.h>
#include <eacp/Core/Plugins/PluginExport.h>
#include <eacp/Core/Utils/Files.h>

#include <string>

namespace
{
const auto roleAtLoad = eacp::Platform::isDLL();

const char* storeIn(std::string& slot, const eacp::FilePath& path)
{
    slot = path.str();
    return slot.c_str();
}
} // namespace

EACP_PLUGIN_EXPORT const void* eacpModuleRoleAddress()
{
    return eacp::Plugins::getCurrentModuleAddress();
}

EACP_PLUGIN_EXPORT int eacpModuleRoleIsDLL()
{
    return eacp::Platform::isDLL() ? 1 : 0;
}

EACP_PLUGIN_EXPORT int eacpModuleRoleIsDLLAtLoad()
{
    return roleAtLoad ? 1 : 0;
}

EACP_PLUGIN_EXPORT const char* eacpModuleRoleCopyPath()
{
    static auto slot = std::string {};
    return storeIn(slot, eacp::Plugins::getCurrentModulePath());
}

EACP_PLUGIN_EXPORT const char* eacpModuleRoleProductPath()
{
    static auto slot = std::string {};
    return storeIn(slot, eacp::Plugins::getProductModulePath());
}

EACP_PLUGIN_EXPORT const char* eacpModuleRoleApplicationPath()
{
    static auto slot = std::string {};
    return storeIn(slot, eacp::Files::applicationPath());
}
