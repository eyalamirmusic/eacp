#include "ModuleInfo.h"
#include "ModuleInfoPlatform.h"

#include <cstdint>
#include <dlfcn.h>
#include <mach-o/loader.h>

namespace eacp::Plugins
{
bool Detail::isInMainProgram(const void* address)
{
    auto info = Dl_info {};

    if (dladdr(address, &info) == 0 || info.dli_fbase == nullptr)
        return true;

    // filetype sits at the same offset in mach_header and mach_header_64.
    return ((const mach_header*) info.dli_fbase)->filetype
           == (std::uint32_t) MH_EXECUTE;
}

void* Detail::findClaimFunction()
{
    return dlsym(RTLD_MAIN_ONLY, "eacpClaimedCopy");
}

FilePath getCurrentModulePath()
{
    return Detail::loadedImagePath();
}
} // namespace eacp::Plugins
