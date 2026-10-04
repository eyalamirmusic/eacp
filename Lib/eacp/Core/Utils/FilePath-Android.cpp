#include "Environment.h"
#include "FilePath.h"
#include "FilePath-Android.h"

namespace eacp
{
namespace
{
std::string& androidDataDirectory()
{
    static auto path = std::string {};
    return path;
}

FilePath androidUnderHome(std::string_view name)
{
    auto home = FilePath::homeDirectory();
    return home.empty() ? home : home / name;
}
} // namespace

void setAndroidDataDirectory(std::string path)
{
    androidDataDirectory() = std::move(path);
}

FilePath FilePath::homeDirectory()
{
    return FilePath {androidDataDirectory()};
}

FilePath FilePath::documentsDirectory()
{
    return androidUnderHome("Documents");
}

FilePath FilePath::downloadsDirectory()
{
    return androidUnderHome("Downloads");
}

FilePath FilePath::musicDirectory()
{
    return androidUnderHome("Music");
}

FilePath FilePath::moviesDirectory()
{
    return androidUnderHome("Videos");
}

FilePath FilePath::picturesDirectory()
{
    return androidUnderHome("Pictures");
}

FilePath FilePath::desktopDirectory()
{
    return androidUnderHome("Desktop");
}

// There is no /tmp on Android.
FilePath FilePath::tempDirectory()
{
    auto path = getEnvValue("TMPDIR");
    return path.empty() ? cacheDirectory() : FilePath {path};
}

FilePath FilePath::appDataDirectory()
{
    return homeDirectory();
}

// Context.getCacheDir(), which the system clears under storage pressure.
FilePath FilePath::cacheDirectory()
{
    auto files = homeDirectory();
    return files.empty() ? files : files.parentDirectory() / "cache";
}
} // namespace eacp
