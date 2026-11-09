#include "TemporaryFile.h"
#include "Files.h"
#include "Format.h"

#include <mutex>
#include <random>
#include <utility>

namespace eacp
{
namespace
{
std::string randomHexToken()
{
    static auto generator = []
    {
        auto device = std::random_device {};
        return std::mt19937_64 {(std::uint64_t {device()} << 32) ^ device()};
    }();

    static auto mutex = std::mutex {};
    auto lock = std::lock_guard {mutex};

    return Strings::format("{:016x}", generator());
}

FilePath unusedTemporaryPath(const FilePath& directory, std::string_view suffix)
{
    while (true)
    {
        auto candidate =
            directory / ("eacp-temp-" + randomHexToken() + std::string {suffix});

        if (!Files::exists(candidate))
            return candidate;
    }
}
} // namespace

TemporaryFile::TemporaryFile(std::string_view suffix)
    : TemporaryFile(FilePath::tempDirectory(), suffix)
{
}

TemporaryFile::TemporaryFile(const FilePath& directory, std::string_view suffix)
    : filePath(unusedTemporaryPath(directory, suffix))
{
}

TemporaryFile::~TemporaryFile()
{
    remove();
}

TemporaryFile::TemporaryFile(TemporaryFile&& other) noexcept
    : filePath(std::exchange(other.filePath, {}))
{
}

TemporaryFile& TemporaryFile::operator=(TemporaryFile&& other) noexcept
{
    if (this != &other)
    {
        remove();
        filePath = std::exchange(other.filePath, {});
    }

    return *this;
}

const FilePath& TemporaryFile::path() const
{
    return filePath;
}

FilePath TemporaryFile::sibling(std::string_view name) const
{
    return filePath.sibling(name);
}

void TemporaryFile::remove()
{
    if (!filePath.empty())
        Files::removeAll(filePath);
}
} // namespace eacp
