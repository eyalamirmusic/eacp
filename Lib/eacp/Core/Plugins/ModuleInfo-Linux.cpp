#include "ModuleInfo.h"

#include <cstdint>
#include <dlfcn.h>
#include <optional>
#include <sys/auxv.h>

namespace eacp::Plugins
{
namespace
{
std::optional<std::uintptr_t> getLoadAddressOfImageContaining(const void* address)
{
    auto info = Dl_info {};

    if (dladdr(address, &info) == 0)
        return std::nullopt;

    return reinterpret_cast<std::uintptr_t>(info.dli_fbase);
}

std::optional<std::uintptr_t> getLoadAddressOfMainProgram()
{
    // The kernel's AT_PHDR points at the main program's own program headers.
    const auto programHeadersAddress = getauxval(AT_PHDR);
    const auto programHeaders = reinterpret_cast<const void*>(programHeadersAddress);

    return getLoadAddressOfImageContaining(programHeaders);
}
} // namespace

bool isDynamicLibrary()
{
    static const auto result = []
    {
        // A PIE executable and a .so are both ET_DYN, so compare load addresses.
        const auto ownCode = reinterpret_cast<const void*>(&isDynamicLibrary);
        const auto ownImage = getLoadAddressOfImageContaining(ownCode);
        const auto mainProgram = getLoadAddressOfMainProgram();

        if (!ownImage || !mainProgram)
            return false;

        return *ownImage != *mainProgram;
    }();

    return result;
}
} // namespace eacp::Plugins
