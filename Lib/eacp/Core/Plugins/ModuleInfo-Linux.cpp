#include "ModuleInfo.h"
#include "ModuleInfoPlatform.h"

#include <dlfcn.h>
#include <filesystem>
#include <optional>
#include <sys/auxv.h>

namespace eacp::Plugins
{
namespace
{
std::optional<const void*> findImageBaseContaining(const void* address)
{
    auto info = Dl_info {};

    if (dladdr(address, &info) == 0)
        return std::nullopt;

    return info.dli_fbase;
}
} // namespace

bool isDynamicLibrary()
{
    static const auto result = []
    {
        // AT_PHDR points into the main program however it was started.
        const auto mainProgramHeaders = (const void*) getauxval(AT_PHDR);
        const auto mainProgramBase = findImageBaseContaining(mainProgramHeaders);
        const auto ownFunction = (const void*) &isDynamicLibrary;
        const auto ownBase = findImageBaseContaining(ownFunction);

        if (!mainProgramBase || !ownBase)
            return false;

        return *mainProgramBase != *ownBase;
    }();

    return result;
}

// glibc names the main program by argv[0], so its path comes from the kernel.
FilePath getCurrentModulePath()
{
    if (isDynamicLibrary())
        return Detail::findLoadedImagePath();

    auto ec = std::error_code();
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", ec);

    if (ec)
        return Detail::findLoadedImagePath();

    return FilePath {executable};
}
} // namespace eacp::Plugins
