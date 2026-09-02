#include "ModuleInfo.h"

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
    auto buffer = std::wstring(MAX_PATH, L'\0');
    auto length = GetModuleFileNameW(
        (HMODULE) getCurrentModuleHandle(), buffer.data(), (DWORD) buffer.size());

    if (length == 0)
        return {};

    buffer.resize(length);

    return FilePath::fromWide(buffer);
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
