#include "ModuleInfo.h"

#include <dlfcn.h>

namespace eacp::Plugins
{
FilePath getCurrentModulePath()
{
    auto info = Dl_info {};

    if (dladdr((const void*) &getCurrentModulePath, &info) != 0
        && info.dli_fname != nullptr)
        return FilePath {info.dli_fname};

    return {};
}

std::string getModuleIdentitySuffix()
{
    auto info = Dl_info {};

    if (dladdr((const void*) &getCurrentModulePath, &info) == 0)
        return {};

    auto value = (std::uintptr_t) info.dli_fbase;
    auto result = std::string();

    while (value != 0)
    {
        result += "0123456789abcdef"[value & 0xf];
        value >>= 4;
    }

    return result;
}
} // namespace eacp::Plugins
