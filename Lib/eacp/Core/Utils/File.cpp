#include "File.h"
#include "StdPath.h"

#include <fstream>

namespace eacp
{
struct File::Impl
{
    explicit Impl(std::filesystem::path path)
        : fsPath(std::move(path))
    {
    }

    std::filesystem::path fsPath;
    std::ifstream stream;
    std::uint64_t position = 0;
};

File::File(FilePath path)
    : filePath(std::move(path))
    , impl(toStdPath(filePath))
{
}

bool File::exists() const
{
    auto ec = std::error_code {};
    return std::filesystem::exists(impl->fsPath, ec);
}

bool File::isRegularFile() const
{
    auto ec = std::error_code {};
    return std::filesystem::is_regular_file(impl->fsPath, ec);
}

bool File::isUnder(const FilePath& root) const
{
    // Canonicalise both sides so the check is symlink-consistent (e.g. macOS
    // /var -> /private/var) regardless of whether the caller pre-normalised.
    auto ec = std::error_code {};
    auto canonicalRoot = std::filesystem::weakly_canonical(toStdPath(root), ec);
    auto canonicalFile = std::filesystem::weakly_canonical(impl->fsPath, ec);
    auto rel = std::filesystem::relative(canonicalFile, canonicalRoot, ec);

    if (ec || rel.empty())
        return false;

    // A path that escapes the root resolves to a relative path starting with
    // "..". Anything else (including ".") is contained.
    return rel.generic_string().rfind("..", 0) != 0;
}

std::uint64_t File::size() const
{
    auto ec = std::error_code {};
    auto bytes = std::filesystem::file_size(impl->fsPath, ec);
    return ec ? 0 : static_cast<std::uint64_t>(bytes);
}

std::int64_t File::modificationTime() const
{
    auto ec = std::error_code {};
    auto written = std::filesystem::last_write_time(impl->fsPath, ec);

    return ec ? 0 : static_cast<std::int64_t>(written.time_since_epoch().count());
}

bool File::openForRead()
{
    if (impl->stream.is_open())
        return true;

    impl->stream.open(impl->fsPath, std::ios::binary);
    impl->position = 0;
    return impl->stream.is_open();
}

bool File::isOpen() const
{
    return impl->stream.is_open();
}

std::size_t File::read(std::uint64_t offset, std::span<std::uint8_t> out)
{
    if (!openForRead() || out.empty())
        return 0;

    auto& stream = impl->stream;

    if (offset != impl->position)
    {
        // Clear any EOF/fail bit left by a previous read before seeking.
        stream.clear();
        stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        impl->position = offset;
    }

    stream.read(reinterpret_cast<char*>(out.data()),
                static_cast<std::streamsize>(out.size()));

    auto got = static_cast<std::size_t>(stream.gcount());
    impl->position += got;
    return got;
}
} // namespace eacp
