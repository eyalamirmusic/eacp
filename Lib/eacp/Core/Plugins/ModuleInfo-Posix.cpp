#include "ModuleInfo.h"
#include "ModuleInfoPlatform.h"

#include <dlfcn.h>
#include <filesystem>

namespace eacp::Plugins
{
namespace
{
// Internal linkage, so no other image can interpose it.
const char moduleAnchor = 0;

const void* imageBaseOf(const void* address)
{
    auto info = Dl_info {};

    if (dladdr(address, &info) == 0)
        return nullptr;

    return info.dli_fbase;
}
} // namespace

const void* getCurrentModuleAddress()
{
    return &moduleAnchor;
}

bool Detail::isSameImage(const void* first, const void* second)
{
    const auto* base = imageBaseOf(first);

    return base != nullptr && base == imageBaseOf(second);
}

FilePath Detail::loadedImagePath()
{
    auto info = Dl_info {};

    if (dladdr(getCurrentModuleAddress(), &info) == 0 || info.dli_fname == nullptr)
        return {};

    auto ec = std::error_code();
    auto canonical = std::filesystem::weakly_canonical(info.dli_fname, ec);

    return FilePath {ec ? std::filesystem::path {info.dli_fname} : canonical};
}

std::string getModuleIdentitySuffix()
{
    auto value = (std::uintptr_t) imageBaseOf(getCurrentModuleAddress());
    auto result = std::string();

    while (value != 0)
    {
        result += "0123456789abcdef"[value & 0xf];
        value >>= 4;
    }

    return result;
}
} // namespace eacp::Plugins
