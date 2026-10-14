#include "ModuleInfo.h"

#include "../Utils/FilesPlatform.h"
#include "../Utils/WinInclude.h"

namespace eacp::Plugins
{
void* getCurrentModuleHandle()
{
    auto module = HMODULE {};

    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                           | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR) &getCurrentModuleHandle,
                       &module);

    return module;
}

bool isDynamicLibrary()
{
    static const auto result =
        getCurrentModuleHandle() != (void*) GetModuleHandleW(nullptr);

    return result;
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
