#include "Files.h"
#include "FilesPlatform.h"
#include "StdPath.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <set>
#include <sstream>
#include <vector>

namespace eacp::Files
{
namespace
{
// The path a save should actually land on: canonical() resolves the whole
// chain, so a symlink is written through rather than replaced. It needs the
// file to exist, so a brand-new file just uses the path as given.
std::filesystem::path resolveForWriting(const std::filesystem::path& path)
{
    auto ec = std::error_code {};
    auto resolved = std::filesystem::canonical(path, ec);

    return ec ? path : resolved;
}

// A free name beside the target. It has to be a sibling rather than something
// under temp_directory_path(): rename is only atomic within one filesystem,
// and /tmp is routinely a different one.
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

// Streaming into an ostringstream and returning its str() is the obvious
// version, and costs four times the file: a doubling buffer plus a copy out.
//
// The size is only a hint: a FIFO or a device has none to give, and on macOS
// file_size throws rather than answering zero, hence the error_code overload.
std::string readFile(const FilePath& path)
{
    const auto stdPath = toStdPath(path);

    auto stream = std::ifstream(stdPath, std::ios::binary);

    if (!stream.is_open())
        return {};

    auto contents = std::string {};
    auto sizeError = std::error_code {};

    if (const auto size = std::filesystem::file_size(stdPath, sizeError); !sizeError)
    {
        contents.resize(static_cast<std::size_t>(size));
        stream.read(contents.data(), static_cast<std::streamsize>(contents.size()));

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

void writeFile(const FilePath& path, Span<const std::uint8_t> bytes)
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

void writeFileAtomically(const FilePath& path, Span<const std::uint8_t> bytes)
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

        // Closed explicitly: the destructor flushes too, but swallows the
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

bool createDirectories(const FilePath& directory)
{
    auto error = std::error_code {};
    std::filesystem::create_directories(toStdPath(directory), error);
    return !error;
}

bool removeAll(const FilePath& path)
{
    auto error = std::error_code {};
    std::filesystem::remove_all(toStdPath(path), error);
    return !error;
}

namespace
{
EntryKind kindOf(const std::filesystem::file_status& status)
{
    if (std::filesystem::is_symlink(status))
        return EntryKind::symlink;

    if (std::filesystem::is_directory(status))
        return EntryKind::directory;

    if (std::filesystem::is_regular_file(status))
        return EntryKind::file;

    return EntryKind::other;
}

bool isHiddenName(const std::filesystem::path& path)
{
    const auto name = path.filename().generic_u8string();
    return !name.empty() && name.front() == u8'.';
}

// One traversal: the options and the visitor for its whole length, and under
// Symlinks::follow the directories already entered.
class DirectoryWalker
{
public:
    DirectoryWalker(const DirectoryOptions& optionsToUse,
                    const VisitingFunc& visitorToUse)
        : options(optionsToUse)
        , visitor(visitorToUse)
    {
    }

    bool walk(const std::filesystem::path& root)
    {
        auto ec = std::error_code {};
        auto iterator = std::filesystem::directory_iterator(root, ec);

        if (ec)
        {
            report(root, "cannot list", ec);
            return false;
        }

        if (options.symlinks == Symlinks::follow)
            rememberEntered(root);

        walkChildren(std::move(iterator), root, 0);
        return true;
    }

private:
    Visit walkChildren(std::filesystem::directory_iterator iterator,
                       const std::filesystem::path& directory,
                       int depth)
    {
        auto [children, listing] =
            collectChildren(std::move(iterator), directory, depth);

        if (listing == Visit::stop)
            return Visit::stop;

        for (const auto& child: children)
        {
            const auto verdict = visitor(child);

            if (verdict == Visit::stop)
                return Visit::stop;

            if (verdict == Visit::next && options.recursive
                && descend(child, depth + 1) == Visit::stop)
                return Visit::stop;
        }

        return Visit::next;
    }

    struct Children
    {
        std::vector<DirectoryEntry> entries;
        Visit listing = Visit::next;
    };

    // The whole directory before any visitor call, so the order is by name and
    // a visitor that deletes what it is shown cannot upset the iterator.
    Children collectChildren(std::filesystem::directory_iterator iterator,
                             const std::filesystem::path& directory,
                             int depth)
    {
        auto children = Children {};
        auto ec = std::error_code {};

        for (; iterator != std::filesystem::directory_iterator {} && !ec;
             iterator.increment(ec))
        {
            if (auto entry = describe(*iterator, depth, children.listing))
                children.entries.push_back(std::move(*entry));

            if (children.listing == Visit::stop)
                return children;
        }

        if (ec && report(directory, "cannot list", ec) == Visit::stop)
            children.listing = Visit::stop;

        std::sort(children.entries.begin(),
                  children.entries.end(),
                  [](const DirectoryEntry& a, const DirectoryEntry& b)
                  { return a.path.str() < b.path.str(); });

        return children;
    }

    std::optional<DirectoryEntry> describe(
        const std::filesystem::directory_entry& item, int depth, Visit& listing)
    {
        auto ec = std::error_code {};
        const auto status = item.symlink_status(ec);

        if (ec)
        {
            listing = report(item.path(), "cannot stat", ec);
            return std::nullopt;
        }

        auto entry = DirectoryEntry {};
        entry.path = FilePath {item.path()};
        entry.kind = kindOf(status);
        entry.depth = depth;
        entry.isHidden =
            isHiddenName(item.path()) || Detail::hasHiddenAttribute(item.path());

        if (entry.isHidden && !options.includeHidden)
            return std::nullopt;

        return entry;
    }

    Visit descend(const DirectoryEntry& entry, int depth)
    {
        const auto path = toStdPath(entry.path);

        if (!leadsToDirectory(entry, path))
            return Visit::next;

        if (options.symlinks == Symlinks::follow && !rememberEntered(path))
            return Visit::next;

        auto ec = std::error_code {};
        auto iterator = std::filesystem::directory_iterator(path, ec);

        if (ec)
            return report(path, "cannot list", ec);

        return walkChildren(std::move(iterator), path, depth);
    }

    bool leadsToDirectory(const DirectoryEntry& entry,
                          const std::filesystem::path& path) const
    {
        if (entry.kind == EntryKind::directory)
            return true;

        if (entry.kind != EntryKind::symlink || options.symlinks != Symlinks::follow)
            return false;

        auto ec = std::error_code {};
        return std::filesystem::is_directory(path, ec);
    }

    // False when this directory has been entered before on this walk.
    bool rememberEntered(const std::filesystem::path& directory)
    {
        auto ec = std::error_code {};
        auto canonical = std::filesystem::canonical(directory, ec);

        if (ec)
            return true;

        return entered.insert(std::move(canonical)).second;
    }

    Visit report(const std::filesystem::path& path,
                 std::string_view operation,
                 const std::error_code& ec)
    {
        auto error = TraversalError {};
        error.path = FilePath {path};
        error.message = std::string {operation} + ": " + ec.message();

        return options.onError(error);
    }

    const DirectoryOptions& options;
    const VisitingFunc& visitor;
    std::set<std::filesystem::path> entered;
};
} // namespace

bool forEachEntry(const FilePath& directory,
                  const DirectoryOptions& options,
                  const VisitingFunc& visitor)
{
    return DirectoryWalker {options, visitor}.walk(toStdPath(directory));
}

Vector<DirectoryEntry> listDirectory(const FilePath& directory,
                                     const DirectoryOptions& options)
{
    auto entries = Vector<DirectoryEntry> {};

    forEachEntry(directory,
                 options,
                 [&](const DirectoryEntry& entry)
                 {
                     entries.add(entry);
                     return Visit::next;
                 });

    return entries;
}

Vector<FilePath> listFiles(const FilePath& directory,
                           const DirectoryOptions& options)
{
    auto files = Vector<FilePath> {};

    const auto isFile = [&](const DirectoryEntry& entry)
    {
        if (entry.kind == EntryKind::file)
            return true;

        if (entry.kind != EntryKind::symlink || options.symlinks != Symlinks::follow)
            return false;

        return entry.file().isRegularFile();
    };

    forEachEntry(directory,
                 options,
                 [&](const DirectoryEntry& entry)
                 {
                     if (isFile(entry))
                         files.add(entry.path);

                     return Visit::next;
                 });

    return files;
}

std::string getBundleResourcePath(const std::string& filename)
{
    if (auto fromBundle = Detail::bundleResourcePath(filename); !fromBundle.empty())
        return fromBundle;

    const auto directory = resourcesDirectory();

    if (directory.empty())
        return {};

    const auto beside = directory / filename;
    auto ec = std::error_code {};

    if (std::filesystem::exists(toStdPath(beside), ec))
        return beside.str();

    return {};
}

std::string filenameFromPath(const std::string& path)
{
    auto separator = path.find_last_of("/\\");

    if (separator != std::string::npos)
        return path.substr(separator + 1);

    return path;
}
} // namespace eacp::Files
