#include "FilePath.h"
#include "WinInclude.h"

#include <KnownFolders.h>
#include <ShlObj.h>
#include <cwchar>
#include <objbase.h>
#include <utility>

namespace eacp
{
namespace
{
// Converts to UTF-8 and normalizes to forward slashes, matching the generic
// style FilePath uses for std::filesystem::path input.
FilePath toFilePath(const wchar_t* text)
{
    if (text == nullptr || *text == L'\0')
        return {};

    auto wideLength = (int) std::wcslen(text);
    auto length = WideCharToMultiByte(
        CP_UTF8, 0, text, wideLength, nullptr, 0, nullptr, nullptr);

    auto result = std::string((std::size_t) length, '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, text, wideLength, result.data(), length, nullptr, nullptr);

    for (auto& character: result)
        if (character == '\\')
            character = '/';

    return FilePath {std::move(result)};
}

FilePath knownFolder(const KNOWNFOLDERID& id)
{
    PWSTR path = nullptr;

    if (SHGetKnownFolderPath(id, 0, nullptr, &path) != S_OK)
    {
        CoTaskMemFree(path);
        return {};
    }

    auto result = toFilePath(path);
    CoTaskMemFree(path);
    return result;
}
} // namespace

FilePath FilePath::homeDirectory()
{
    return knownFolder(FOLDERID_Profile);
}

FilePath FilePath::documentsDirectory()
{
    return knownFolder(FOLDERID_Documents);
}

FilePath FilePath::downloadsDirectory()
{
    return knownFolder(FOLDERID_Downloads);
}

FilePath FilePath::musicDirectory()
{
    return knownFolder(FOLDERID_Music);
}

FilePath FilePath::moviesDirectory()
{
    return knownFolder(FOLDERID_Videos);
}

FilePath FilePath::picturesDirectory()
{
    return knownFolder(FOLDERID_Pictures);
}

FilePath FilePath::desktopDirectory()
{
    return knownFolder(FOLDERID_Desktop);
}

FilePath FilePath::tempDirectory()
{
    wchar_t buffer[MAX_PATH + 2] {};

    if (GetTempPathW(MAX_PATH + 2, buffer) == 0)
        return {};

    auto path = toFilePath(buffer).str();

    if (!path.empty() && path.back() == '/')
        path.pop_back();

    return FilePath {std::move(path)};
}

FilePath FilePath::appDataDirectory()
{
    return knownFolder(FOLDERID_RoamingAppData);
}

FilePath FilePath::cacheDirectory()
{
    return knownFolder(FOLDERID_LocalAppData);
}
} // namespace eacp
