#include "FilePath.h"

#include "../Platform/Platform.h"
#include "Files.h"
#include "Strings.h"

#include <cstddef>
#include <type_traits>

namespace eacp
{
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

namespace
{
std::string executableName()
{
    auto name = Files::filenameFromPath(Files::executablePath().str());
    auto dot = name.find_last_of('.');

    if (dot != std::string::npos && dot > 0)
        name.erase(dot);

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

void FilePath::assignFromWide(std::wstring_view wide)
{
    text = Strings::narrow(wide);

    // '\' is ASCII and no UTF-8 continuation byte can be 0x5C, so swapping the
    // byte hits exactly the separators and nothing inside a multi-byte sequence.
    for (auto& character: text)
        if (character == '\\')
            character = '/';
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
