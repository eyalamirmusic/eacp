#include "File.h"

namespace eacp
{
File::File(std::filesystem::path path)
    : filePath(std::move(path))
{
}

bool File::exists() const
{
    auto ec = std::error_code {};
    return std::filesystem::exists(filePath, ec);
}

bool File::isRegularFile() const
{
    auto ec = std::error_code {};
    return std::filesystem::is_regular_file(filePath, ec);
}

std::uint64_t File::size() const
{
    auto ec = std::error_code {};
    auto bytes = std::filesystem::file_size(filePath, ec);
    return ec ? 0 : static_cast<std::uint64_t>(bytes);
}

bool File::openForRead()
{
    if (stream.is_open())
        return true;

    stream.open(filePath, std::ios::binary);
    position = 0;
    return stream.is_open();
}

std::size_t File::read(std::uint64_t offset, std::span<std::uint8_t> out)
{
    if (!openForRead() || out.empty())
        return 0;

    if (offset != position)
    {
        // Clear any EOF/fail bit left by a previous read before seeking.
        stream.clear();
        stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        position = offset;
    }

    stream.read(reinterpret_cast<char*>(out.data()),
                static_cast<std::streamsize>(out.size()));

    auto got = static_cast<std::size_t>(stream.gcount());
    position += got;
    return got;
}
} // namespace eacp
