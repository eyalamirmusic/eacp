#include "FilePath.h"

#include "../Platform/Platform.h"
#include "Files.h"
#include "FilesPlatform.h"
#include "StdPath.h"
#include "Strings.h"

#include <cstddef>
#include <type_traits>

namespace eacp
{
std::filesystem::path toStdPath(const FilePath& path)
{
    // Wide on Windows via wide(), which never throws — the u8string route
    // throws on text that is not valid UTF-8. Elsewhere the native encoding
    // is the text as-is.
    if constexpr (sizeof(std::filesystem::path::value_type) == sizeof(wchar_t))
        return std::filesystem::path {path.wide()};
    else
        return std::filesystem::path {path.str()};
}

FilePath::FilePath(std::string textToUse)
    : text(std::move(textToUse))
{
}

FilePath::FilePath(std::string_view textToUse)
    : text(textToUse)
{
}

FilePath::FilePath(const char* textToUse)
    : text(textToUse)
{
}

const std::string& FilePath::str() const
{
    return text;
}

const char* FilePath::c_str() const
{
    return text.c_str();
}

bool FilePath::empty() const
{
    return text.empty();
}

std::string Detail::replaceSeparators(std::string_view text, char from, char to)
{
    auto result = std::string {text};

    for (auto& character: result)
        if (character == from)
            character = to;

    return result;
}

std::string FilePath::nativeString() const
{
    if constexpr (Platform::isWindows())
        return Detail::replaceSeparators(text, '/', '\\');

    return text;
}

FilePath FilePath::fromNative(std::string_view nativeText)
{
    if constexpr (Platform::isWindows())
        return FilePath {Detail::replaceSeparators(nativeText, '\\', '/')};

    return FilePath {nativeText};
}

std::string FilePath::extension() const
{
    auto separator = text.find_last_of("/\\");
    auto start = separator == std::string::npos ? 0 : separator + 1;
    auto dot = text.find_last_of('.');

    if (dot == std::string::npos || dot <= start)
        return {};

    return text.substr(dot);
}

FilePath FilePath::parentDirectory() const
{
    auto separator = text.find_last_of("/\\");

    if (separator == std::string::npos)
        return {};

    if (separator == 0)
        return FilePath {"/"};

    return FilePath {text.substr(0, separator)};
}

std::string FilePath::fileName() const
{
    auto separator = text.find_last_of("/\\");
    return separator == std::string::npos ? text : text.substr(separator + 1);
}

std::string FilePath::stem() const
{
    auto name = fileName();
    auto dot = name.find_last_of('.');

    if (dot == std::string::npos || dot == 0)
        return name;

    return name.substr(0, dot);
}

namespace
{
bool endsWithCaseInsensitive(std::string_view text, std::string_view suffix)
{
    return text.size() >= suffix.size()
           && Strings::equalsCaseInsensitive(
               text.substr(text.size() - suffix.size()), suffix);
}

// One alternative of hasExtension, matched against the file name the way
// JUCE's hasFileExtension does: "wav" must follow a dot, ".wav" carries its
// own, so "tar.gz" matches "a.tar.gz" too.
bool nameHasExtension(std::string_view name, std::string_view ext)
{
    // No extension, in extension()'s sense: a dotfile's leading dot is part
    // of its name.
    if (ext.empty())
    {
        const auto dot = name.find_last_of('.');
        return dot == std::string_view::npos || dot == 0;
    }

    if (!endsWithCaseInsensitive(name, ext))
        return false;

    if (ext.front() == '.')
        return true;

    return name.size() > ext.size() && name[name.size() - ext.size() - 1] == '.';
}
} // namespace

bool FilePath::hasExtension(std::string_view ext) const
{
    const auto name = fileName();

    if (ext.find(';') == std::string_view::npos)
        return nameHasExtension(name, Strings::trim(ext));

    for (auto rest = ext;;)
    {
        auto semicolon = rest.find(';');
        auto alternative = Strings::trim(rest.substr(0, semicolon));

        if (!alternative.empty() && nameHasExtension(name, alternative))
            return true;

        if (semicolon == std::string_view::npos)
            return false;

        rest.remove_prefix(semicolon + 1);
    }
}

FilePath FilePath::withExtension(std::string_view ext) const
{
    if (text.empty())
        return {};

    auto name = stem();

    if (!ext.empty() && ext.front() != '.')
        name += '.';

    name += ext;
    return sibling(name);
}

FilePath FilePath::sibling(std::string_view name) const
{
    auto parent = parentDirectory();
    return parent.empty() ? FilePath {name} : parent / name;
}

bool FilePath::isAbsolute() const
{
    if (text.empty())
        return false;

    if (text.front() == '/')
        return true;

    if constexpr (Platform::isWindows())
        return text.front() == '\\'
               || (text.size() >= 3 && text[1] == ':'
                   && (text[2] == '/' || text[2] == '\\'));

    return false;
}

namespace
{
std::string executableName()
{
    auto name = Files::filenameFromPath(Files::executablePath().str());
    auto dot = name.find_last_of('.');

    if (dot != std::string::npos && dot > 0)
        name.erase(dot);

    if constexpr (Platform::isAndroid())
        if (name.starts_with("lib") && name.size() > 3)
            name.erase(0, 3);

    return name;
}

std::string currentAppName()
{
    if (auto name = Platform::getAppName(); !name.empty())
        return std::string {name};

    if (auto name = executableName(); !name.empty())
        return name;

    return "eacp";
}

FilePath appFolderUnder(const FilePath& root,
                        std::string_view company,
                        std::string_view app)
{
    auto folder = company.empty() ? root : root / company;
    return app.empty() ? folder : folder / app;
}
} // namespace

FilePath FilePath::appSupportDirectory()
{
    return appSupportDirectory(Platform::getCompanyName(), currentAppName());
}

FilePath FilePath::appCacheDirectory()
{
    return appCacheDirectory(Platform::getCompanyName(), currentAppName());
}

FilePath FilePath::appSupportDirectory(std::string_view company,
                                       std::string_view app)
{
    return appFolderUnder(appDataDirectory(), company, app);
}

FilePath FilePath::appCacheDirectory(std::string_view company, std::string_view app)
{
    return appFolderUnder(cacheDirectory(), company, app);
}

std::wstring FilePath::wide() const
{
    return Strings::widen(text);
}

FilePath FilePath::fromWide(std::wstring_view wide)
{
    auto path = FilePath {};
    path.assignFromWide(wide);
    return path;
}

std::string FilePath::read() const
{
    return Files::readFile(*this);
}

void FilePath::assignFromWide(std::wstring_view wide)
{
    // '\' is ASCII and no UTF-8 continuation byte can be 0x5C, so swapping the
    // byte hits exactly the separators and nothing inside a multi-byte sequence.
    text = Detail::replaceSeparators(Strings::narrow(wide), '\\', '/');
}

FilePath FilePath::operator/(std::string_view part) const
{
    auto joined = text;
    if (!joined.empty() && joined.back() != '/')
        joined += '/';

    joined += part;
    return FilePath {std::move(joined)};
}
} // namespace eacp
