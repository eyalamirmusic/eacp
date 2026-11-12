#include "ModuleInfo.h"
#include "ModuleInfoPlatform.h"

#include "../Utils/FilesPlatform.h"
#include "../Utils/WinInclude.h"

namespace eacp::Plugins
{
namespace
{
HMODULE moduleContaining(const void* address)
{
    auto module = HMODULE {};

    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                           | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR) address,
                       &module);

    return module;
}
} // namespace

bool Detail::isInMainProgram(const void* address)
{
    return moduleContaining(address) == GetModuleHandleW(nullptr);
}

bool Detail::isSameImage(const void* first, const void* second)
{
    auto module = moduleContaining(first);

    return module != nullptr && module == moduleContaining(second);
}

void* Detail::findClaimFunction()
{
    return (void*) GetProcAddress(GetModuleHandleW(nullptr), "eacpClaimedCopy");
}

void* getCurrentModuleHandle()
{
    return moduleContaining((const void*) &getCurrentModuleHandle);
}

const void* getCurrentModuleAddress()
{
    return getCurrentModuleHandle();
}

void* getProductModuleHandle()
{
    if (isDynamicLibrary())
        return getCurrentModuleHandle();

    return GetModuleHandleW(nullptr);
}

FilePath getCurrentModulePath()
{
    return eacp::Detail::modulePath(getCurrentModuleHandle());
}

std::string getModuleIdentitySuffix()
{
    auto value = (std::uintptr_t) getCurrentModuleHandle();
    auto result = std::string();

    while (value != 0)
    {
        result += "0123456789abcdef"[value & 0xf];
        value >>= 4;
    }

    return result;
}

std::wstring getUniqueWindowClassName(const wchar_t* root)
{
    auto suffix = getModuleIdentitySuffix();
    auto result = std::wstring(root) + L"_";

    for (auto c: suffix)
        result += (wchar_t) c;

    return result;
}
} // namespace eacp::Plugins
