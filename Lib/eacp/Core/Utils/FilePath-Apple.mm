#include "FilePath.h"

#include "../ObjC/AutoReleasePool.h"
#include "../ObjC/Strings.h"

#include <utility>

namespace eacp
{
namespace
{
FilePath searchPathDirectory(NSSearchPathDirectory directory)
{
    auto pool = ObjC::AutoReleasePool();
    auto* paths =
        NSSearchPathForDirectoriesInDomains(directory, NSUserDomainMask, YES);

    if (paths.count == 0)
        return {};

    return FilePath {Strings::toStdString(paths.firstObject)};
}
} // namespace

FilePath FilePath::homeDirectory()
{
    auto pool = ObjC::AutoReleasePool();
    return FilePath {Strings::toStdString(NSHomeDirectory())};
}

FilePath FilePath::documentsDirectory()
{
    return searchPathDirectory(NSDocumentDirectory);
}

FilePath FilePath::downloadsDirectory()
{
    return searchPathDirectory(NSDownloadsDirectory);
}

FilePath FilePath::musicDirectory()
{
    return searchPathDirectory(NSMusicDirectory);
}

FilePath FilePath::moviesDirectory()
{
    return searchPathDirectory(NSMoviesDirectory);
}

FilePath FilePath::picturesDirectory()
{
    return searchPathDirectory(NSPicturesDirectory);
}

FilePath FilePath::desktopDirectory()
{
    return searchPathDirectory(NSDesktopDirectory);
}

FilePath FilePath::tempDirectory()
{
    auto pool = ObjC::AutoReleasePool();
    auto path = Strings::toStdString(NSTemporaryDirectory());

    if (!path.empty() && path.back() == '/')
        path.pop_back();

    return FilePath {std::move(path)};
}

FilePath FilePath::appDataDirectory()
{
    return searchPathDirectory(NSApplicationSupportDirectory);
}

FilePath FilePath::cacheDirectory()
{
    return searchPathDirectory(NSCachesDirectory);
}
} // namespace eacp
