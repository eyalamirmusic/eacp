#include "FallbackReport.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace eacp::CrashReporting::Detail
{
namespace
{
constexpr auto reportFolderMode = mode_t {0755};
constexpr auto reportFileMode = mode_t {0644};
} // namespace

CrashTimePath makeCrashTimePath(const FilePath& path)
{
    return path.str();
}

bool createCrashTimeDirectory(const CrashTimePath& path)
{
    return ::mkdir(path.c_str(), reportFolderMode) == 0 || errno == EEXIST;
}

bool writeCrashTimeFile(const CrashTimePath& path, std::string_view bytes)
{
    auto descriptor = ::open(
        path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, reportFileMode);

    if (descriptor < 0)
        return false;

    auto remaining = bytes;

    while (!remaining.empty())
    {
        auto written = ::write(descriptor, remaining.data(), remaining.size());

        if (written < 0 && errno == EINTR)
            continue;

        if (written <= 0)
            break;

        remaining.remove_prefix(static_cast<std::size_t>(written));
    }

    return ::close(descriptor) == 0 && remaining.empty();
}

bool renameCrashTimeFile(const CrashTimePath& from, const CrashTimePath& to)
{
    return ::rename(from.c_str(), to.c_str()) == 0;
}
} // namespace eacp::CrashReporting::Detail
