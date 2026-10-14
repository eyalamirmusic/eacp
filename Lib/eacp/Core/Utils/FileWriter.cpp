#include "FileWriter.h"
#include "StdPath.h"

#include <fstream>

// std::fstream rather than a FILE* or a native handle: it takes a
// std::filesystem::path, which is wide on Windows, so UTF-8 names open on every
// platform from this one file, as they do for File's ifstream. Opened in+out
// (never ios::app, which would pin every write to the end) so seek() can
// patch bytes that were written earlier.
namespace eacp
{
namespace
{
bool createFileIfMissing(const std::filesystem::path& path)
{
    auto ec = std::error_code {};

    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), ec);

    if (std::filesystem::exists(path, ec))
        return true;

    return static_cast<bool>(std::ofstream(path, std::ios::binary));
}
} // namespace

struct FileWriter::Impl
{
    Impl(std::filesystem::path pathToUse, Mode mode)
        : fsPath(std::move(pathToUse))
    {
        if (!createFileIfMissing(fsPath))
            return;

        auto flags = std::ios::binary | std::ios::in | std::ios::out;

        if (mode == Mode::truncate)
            flags |= std::ios::trunc;

        stream.open(fsPath, flags);

        if (mode == Mode::append)
            stream.seekp(0, std::ios::end);
    }

    std::filesystem::path fsPath;

    // tellp() is non-const, and position() is a query.
    mutable std::fstream stream;
};

FileWriter::FileWriter(const FilePath& path, Mode mode)
    : filePath(path)
    , impl(toStdPath(path), mode)
{
}

const FilePath& FileWriter::path() const
{
    return filePath;
}

bool FileWriter::isOpen() const
{
    return impl->stream.is_open();
}

bool FileWriter::write(Span<const std::uint8_t> bytes)
{
    if (!isOpen())
        return false;

    impl->stream.write(reinterpret_cast<const char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));

    return static_cast<bool>(impl->stream);
}

bool FileWriter::write(std::string_view text)
{
    return write(Span {reinterpret_cast<const std::uint8_t*>(text.data()),
                       (int) text.size()});
}

bool FileWriter::seek(std::uint64_t offset)
{
    if (!isOpen())
        return false;

    impl->stream.clear();
    impl->stream.seekp(static_cast<std::streamoff>(offset), std::ios::beg);

    return static_cast<bool>(impl->stream);
}

std::uint64_t FileWriter::position() const
{
    if (!isOpen())
        return 0;

    auto position = impl->stream.tellp();
    return position < 0 ? 0 : static_cast<std::uint64_t>(position);
}

bool FileWriter::flush()
{
    if (!isOpen())
        return false;

    impl->stream.flush();
    return static_cast<bool>(impl->stream);
}

bool FileWriter::truncate()
{
    if (!flush())
        return false;

    auto ec = std::error_code {};
    std::filesystem::resize_file(impl->fsPath, position(), ec);

    return !ec;
}
} // namespace eacp
