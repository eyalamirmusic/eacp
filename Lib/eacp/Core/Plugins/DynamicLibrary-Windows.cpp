#include "DynamicLibrary.h"
#include "DynamicLibraryPlatform.h"

#include "../Utils/WinInclude.h"

namespace eacp::Plugins
{
namespace Detail
{
void* loadImage(const FilePath& path)
{
    return LoadLibraryW(path.wide().c_str());
}

std::string lastLoadError()
{
    auto code = GetLastError();
    char* buffer = nullptr;
    auto flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                 | FORMAT_MESSAGE_IGNORE_INSERTS;
    auto length =
        FormatMessageA(flags, nullptr, code, 0, (LPSTR) &buffer, 0, nullptr);
    auto message = length == 0 ? std::string {} : std::string(buffer, length);
    LocalFree(buffer);

    while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
        message.pop_back();

    return "error " + std::to_string(code) + ": " + message;
}

void unloadImage(void* handle)
{
    FreeLibrary((HMODULE) handle);
}

void* findImageSymbol(void* handle, const std::string& name)
{
    return (void*) GetProcAddress((HMODULE) handle, name.c_str());
}
} // namespace Detail

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
