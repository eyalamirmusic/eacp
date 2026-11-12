// Fixture for ModuleRoleTests: a shared library carrying its own eacp copy,
// reporting what that copy answers. Built twice, as ModuleRoleLibraryA (the
// one ModuleRoleLauncher links and claims) and ModuleRoleLibraryB (one it only
// loads). The role is also taken by a static initialiser, which on every
// platform runs before the launcher's own C runtime is up.
#include <eacp/Core/Platform/Platform.h>
#include <eacp/Core/Plugins/ModuleInfo.h>
#include <eacp/Core/Plugins/PluginExport.h>
#include <eacp/Core/Utils/Files.h>

#include <string>

namespace
{
const auto roleAtLoad = eacp::Platform::isDLL();

const char* keep(std::string& slot, const eacp::FilePath& path)
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
    return keep(slot, eacp::Plugins::getCurrentModulePath());
}

EACP_PLUGIN_EXPORT const char* eacpModuleRoleProductPath()
{
    static auto slot = std::string {};
    return keep(slot, eacp::Plugins::getProductModulePath());
}

EACP_PLUGIN_EXPORT const char* eacpModuleRoleApplicationPath()
{
    static auto slot = std::string {};
    return keep(slot, eacp::Files::applicationPath());
}
