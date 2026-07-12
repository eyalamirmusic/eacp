#include "DynamicLibrary.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace eacp::Plugins
{
namespace
{
std::wstring toWide(const std::string& text)
{
    if (text.empty())
        return {};

    auto length =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), (int) text.size(), nullptr, 0);

    auto result = std::wstring((std::size_t) length, L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, text.data(), (int) text.size(), result.data(), length);

    return result;
}
} // namespace

DynamicLibrary::DynamicLibrary(const FilePath& path)
{
    open(path);
}

DynamicLibrary::~DynamicLibrary()
{
    close();
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept
    : handle(std::exchange(other.handle, nullptr))
{
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept
{
    if (this != &other)
    {
        close();
        handle = std::exchange(other.handle, nullptr);
    }

    return *this;
}

bool DynamicLibrary::open(const FilePath& path)
{
    close();
    handle = LoadLibraryW(toWide(path.str()).c_str());
    return handle != nullptr;
}

void DynamicLibrary::close()
{
    if (handle != nullptr)
    {
        FreeLibrary((HMODULE) handle);
        handle = nullptr;
    }
}

bool DynamicLibrary::isOpen() const
{
    return handle != nullptr;
}

void* DynamicLibrary::findSymbol(const std::string& name) const
{
    if (handle == nullptr)
        return nullptr;

    return (void*) GetProcAddress((HMODULE) handle, name.c_str());
}

Vector<std::string> DynamicLibrary::getFunctionNames() const
{
    auto result = Vector<std::string>();

    if (handle == nullptr)
        return result;

    auto* base = (const std::uint8_t*) handle;
    auto* dosHeader = (const IMAGE_DOS_HEADER*) base;

    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
        return result;

    auto* ntHeaders = (const IMAGE_NT_HEADERS*) (base + dosHeader->e_lfanew);

    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
        return result;

    auto& directory =
        ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];

    if (directory.Size == 0)
        return result;

    auto* exports =
        (const IMAGE_EXPORT_DIRECTORY*) (base + directory.VirtualAddress);
    auto* names = (const DWORD*) (base + exports->AddressOfNames);

    for (auto i = DWORD {0}; i < exports->NumberOfNames; ++i)
        result.add((const char*) (base + names[i]));

    return result;
}
} // namespace eacp::Plugins
