#include "ModuleInfo.h"

#include <cstdint>
#include <dlfcn.h>
#include <mach-o/loader.h>

namespace eacp::Plugins
{
bool isDynamicLibrary()
{
    static const auto result = []
    {
        auto info = Dl_info {};

        if (dladdr((const void*) &isDynamicLibrary, &info) == 0
            || info.dli_fbase == nullptr)
            return false;

        // filetype sits at the same offset in mach_header and mach_header_64.
        return ((const mach_header*) info.dli_fbase)->filetype
               != (std::uint32_t) MH_EXECUTE;
    }();

    return result;
}
} // namespace eacp::Plugins
