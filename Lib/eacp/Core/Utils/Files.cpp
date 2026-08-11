#include "Files.h"
#include "StdPath.h"

#include <atomic>
#include <fstream>
#include <sstream>

namespace eacp::Files
{
namespace
{
// canonical() resolves the whole chain, so a symlink is written through rather
// than replaced. It needs the file to exist, so a new file uses the path as-is.
std::filesystem::path resolveForWriting(const std::filesystem::path& path)
{
    auto ec = std::error_code {};
    auto resolved = std::filesystem::canonical(path, ec);

    return ec ? path : resolved;
}

// A sibling rather than something under temp_directory_path(): rename is only
// atomic within one filesystem, and /tmp is routinely a different one.
std::filesystem::path temporaryBeside(const std::filesystem::path& target)
{
    static auto counter = std::atomic<unsigned> {0};

    for (auto attempt = 0; attempt < 64; ++attempt)
    {
        auto candidate = target;
        candidate += ".eacp-tmp-" + std::to_string(counter.fetch_add(1));

        auto ec = std::error_code {};

        if (!std::filesystem::exists(candidate, ec))
            return candidate;
    }

    throw std::runtime_error("no free temporary name beside '" + target.string()
                             + "'");
}
} // namespace

// The size is only a hint: a FIFO has none to give (macOS file_size throws
// rather than answering zero, hence the error_code overload), and the stream
// stays in text mode, so Windows reads a CRLF pair as one character.
std::string readFile(const FilePath& path)
{
    const auto stdPath = toStdPath(path);

    auto stream = std::ifstream(stdPath);

    if (!stream.is_open())
        return {};

    auto contents = std::string {};
    auto sizeError = std::error_code {};

    if (const auto size = std::filesystem::file_size(stdPath, sizeError); !sizeError)
    {
        contents.resize(static_cast<std::size_t>(size));
        stream.read(contents.data(), static_cast<std::streamsize>(contents.size()));

        // Not resize(size), which on Windows leaves a run of NULs after the text.
        contents.resize(static_cast<std::size_t>(stream.gcount()));
    }

    if (stream && stream.peek() != std::char_traits<char>::eof())
    {
        auto rest = std::ostringstream {};
        rest << stream.rdbuf();
        contents += rest.str();
    }

    return contents;
}

void writeFile(const FilePath& path, std::span<const std::uint8_t> bytes)
{
    auto fsPath = toStdPath(path);

    if (fsPath.has_parent_path())
        std::filesystem::create_directories(fsPath.parent_path());

    auto stream = std::ofstream(fsPath, std::ios::binary | std::ios::trunc);
    if (!stream)
        throw std::runtime_error("cannot open '" + path.str() + "' for writing");

    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));

    if (!stream)
        throw std::runtime_error("cannot write '" + path.str() + "'");
}

void writeFileAtomically(const FilePath& path, std::span<const std::uint8_t> bytes)
{
    auto fsPath = toStdPath(path);

    if (fsPath.has_parent_path())
        std::filesystem::create_directories(fsPath.parent_path());

    auto target = resolveForWriting(fsPath);
    auto temporary = temporaryBeside(target);

    auto abandon = [&](const std::string& what)
    {
        auto ec = std::error_code {};
        std::filesystem::remove(temporary, ec);

        return std::runtime_error(what + " '" + path.str() + "'");
    };

    {
        auto stream = std::ofstream(temporary, std::ios::binary | std::ios::trunc);

        if (!stream)
            throw abandon("cannot open a temporary file beside");

        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));

        // Closed explicitly: the destructor also flushes, but swallows the
        // failure, and a full disk shows up here or nowhere.
        stream.close();

        if (!stream)
            throw abandon("cannot write");
    }

    auto ec = std::error_code {};

    if (auto existing = std::filesystem::status(target, ec); !ec)
        std::filesystem::permissions(temporary, existing.permissions(), ec);

    ec.clear();
    std::filesystem::rename(temporary, target, ec);

    if (ec)
        throw abandon("cannot replace");
}

std::string filenameFromPath(const std::string& path)
{
    auto separator = path.find_last_of("/\\");

    if (separator != std::string::npos)
        return path.substr(separator + 1);

    return path;
}
} // namespace eacp::Files
