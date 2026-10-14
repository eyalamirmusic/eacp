#include "Files.h"
#include "FilesPlatform.h"
#include "StdPath.h"

#include "../Platform/Platform.h"
#include "../Plugins/ModuleInfo.h"

#include <algorithm>
#include <atomic>
#include <charconv>
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

bool exists(const FilePath& path)
{
    auto ec = std::error_code {};
    return std::filesystem::exists(toStdPath(path), ec);
}

bool isFile(const FilePath& path)
{
    auto ec = std::error_code {};
    return std::filesystem::is_regular_file(toStdPath(path), ec);
}

bool isDirectory(const FilePath& path)
{
    auto ec = std::error_code {};
    return std::filesystem::is_directory(toStdPath(path), ec);
}

namespace
{
// Anything at all at path, a dangling symlink included: what a copy or a move
// would have to replace, and what a fresh name must not collide with.
bool somethingAt(const std::filesystem::path& path)
{
    auto ec = std::error_code {};
    return std::filesystem::exists(std::filesystem::symlink_status(path, ec));
}

bool createParentOf(const std::filesystem::path& path)
{
    if (!path.has_parent_path())
        return true;

    auto ec = std::error_code {};
    std::filesystem::create_directories(path.parent_path(), ec);
    return !ec;
}

bool isSameEntry(const std::filesystem::path& a, const std::filesystem::path& b)
{
    auto ec = std::error_code {};
    return std::filesystem::equivalent(a, b, ec) && !ec;
}

// True when `inner` is `outer` or lies somewhere below it, so a recursive
// copy of outer into inner would never end.
bool isWithin(const std::filesystem::path& inner, const std::filesystem::path& outer)
{
    auto innerError = std::error_code {};
    auto outerError = std::error_code {};
    auto canonicalInner = std::filesystem::weakly_canonical(inner, innerError);
    auto canonicalOuter = std::filesystem::weakly_canonical(outer, outerError);

    if (innerError || outerError)
        return false;

    auto relative = canonicalInner.lexically_relative(canonicalOuter);
    return !relative.empty() && *relative.begin() != "..";
}

bool copyEntry(const std::filesystem::path& from, const std::filesystem::path& to)
{
    auto ec = std::error_code {};
    const auto status = std::filesystem::symlink_status(from, ec);

    if (ec)
        return false;

    if (std::filesystem::is_symlink(status))
        std::filesystem::copy_symlink(from, to, ec);
    else if (std::filesystem::is_directory(status))
        std::filesystem::copy(from,
                              to,
                              std::filesystem::copy_options::recursive
                                  | std::filesystem::copy_options::copy_symlinks,
                              ec);
    else
        std::filesystem::copy_file(
            from, to, std::filesystem::copy_options::overwrite_existing, ec);

    return !ec;
}

// The rename errors that mean "something is already at the target", as
// opposed to a missing permission or a full disk. Windows reports a
// directory in the way as access denied.
bool isTargetInTheWay(const std::error_code& ec)
{
    if (ec == std::errc::file_exists || ec == std::errc::directory_not_empty
        || ec == std::errc::is_a_directory)
        return true;

    if constexpr (Platform::isWindows())
        return ec == std::errc::permission_denied;

    return false;
}

bool writeBytes(std::ostream& stream, Span<const std::uint8_t> bytes)
{
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    stream.flush();

    return static_cast<bool>(stream);
}
} // namespace

bool copyFile(const FilePath& from, const FilePath& to)
{
    const auto source = toStdPath(from);
    const auto target = toStdPath(to);

    if (!isFile(from) || !createParentOf(target))
        return false;

    if (isSameEntry(source, target))
        return true;

    auto ec = std::error_code {};
    std::filesystem::copy_file(
        source, target, std::filesystem::copy_options::overwrite_existing, ec);

    return !ec;
}

bool copyDirectory(const FilePath& from, const FilePath& to)
{
    const auto source = toStdPath(from);
    const auto target = toStdPath(to);

    // Either way round the replace below would delete the source.
    if (!isDirectory(from) || isWithin(target, source) || isWithin(source, target))
        return false;

    if (!removeAll(to) || !createParentOf(target))
        return false;

    auto ec = std::error_code {};
    std::filesystem::copy(source,
                          target,
                          std::filesystem::copy_options::recursive
                              | std::filesystem::copy_options::copy_symlinks,
                          ec);

    return !ec;
}

bool moveFile(const FilePath& from, const FilePath& to)
{
    const auto source = toStdPath(from);
    const auto target = toStdPath(to);

    if (!somethingAt(source))
        return false;

    if (from == to)
        return true;

    // A case-only rename on a case-insensitive volume is the same entry, and
    // neither inside nor above itself.
    const auto sameEntry = isSameEntry(source, target);

    // Onto one of its own ancestors: replacing the target would delete the
    // source with it.
    if (!sameEntry && isWithin(source, target))
        return false;

    // Into its own subtree. symlink_status: a link is moved, not followed.
    auto statusError = std::error_code {};
    const auto sourceIsDirectory = std::filesystem::is_directory(
        std::filesystem::symlink_status(source, statusError));

    if (sourceIsDirectory && !sameEntry && isWithin(target, source))
        return false;

    if (!createParentOf(target))
        return false;

    auto ec = std::error_code {};
    std::filesystem::rename(source, target, ec);

    if (!ec)
        return true;

    // A rename does not replace a non-empty directory (nor, on Windows, a
    // directory at all), so clear the way and try once more - but only when
    // the target is what stopped it.
    if (isTargetInTheWay(ec) && somethingAt(target) && !sameEntry)
    {
        if (!removeAll(to))
            return false;

        ec.clear();
        std::filesystem::rename(source, target, ec);

        if (!ec)
            return true;
    }

    // Different filesystems: no rename can do it, so copy and remove. Any
    // other failure leaves both sides as they are.
    if (ec != std::errc::cross_device_link)
        return false;

    if (!removeAll(to) || !copyEntry(source, target))
    {
        removeAll(to);
        return false;
    }

    return removeAll(from);
}

bool removeFile(const FilePath& path)
{
    const auto fsPath = toStdPath(path);
    auto ec = std::error_code {};
    const auto status = std::filesystem::symlink_status(fsPath, ec);

    if (ec || !std::filesystem::exists(status)
        || std::filesystem::is_directory(status))
        return false;

    return std::filesystem::remove(fsPath, ec) && !ec;
}

bool appendFile(const FilePath& path, Span<const std::uint8_t> bytes)
{
    const auto fsPath = toStdPath(path);

    if (!createParentOf(fsPath))
        return false;

    auto stream = std::ofstream(fsPath, std::ios::binary | std::ios::app);
    return stream && writeBytes(stream, bytes);
}

bool appendFile(const FilePath& path, std::string_view text)
{
    return appendFile(path,
                      Span {reinterpret_cast<const std::uint8_t*>(text.data()),
                            (int) text.size()});
}

bool writeFileAt(const FilePath& path,
                 std::uint64_t offset,
                 Span<const std::uint8_t> bytes)
{
    const auto fsPath = toStdPath(path);

    if (!createParentOf(fsPath))
        return false;

    if (!somethingAt(fsPath) && !std::ofstream(fsPath, std::ios::binary))
        return false;

    auto stream =
        std::fstream(fsPath, std::ios::binary | std::ios::in | std::ios::out);

    if (!stream)
        return false;

    stream.seekp(static_cast<std::streamoff>(offset));
    return stream && writeBytes(stream, bytes);
}

namespace
{
// Byte offsets where each UTF-8 code point of text begins, so lengths and cuts
// count characters rather than bytes, as JUCE's String does.
std::vector<std::size_t> codePointStarts(std::string_view text)
{
    auto starts = std::vector<std::size_t> {};

    // The first byte always starts a character, even a stray continuation
    // byte, so no byte is ever skipped over.
    for (auto index = std::size_t {0}; index < text.size(); ++index)
        if (index == 0 || (static_cast<unsigned char>(text[index]) & 0xC0) != 0x80)
            starts.push_back(index);

    return starts;
}

bool isIllegalInFileName(char character)
{
    return std::string_view {"\"#@,;:<>*^|?\\/"}.find(character)
           != std::string_view::npos;
}
} // namespace

std::string legalFileName(std::string_view name)
{
    constexpr auto maxLength = std::size_t {128};

    auto legal = std::string {};

    for (auto character: name)
        if (!isIllegalInFileName(character))
            legal += character;

    const auto starts = codePointStarts(legal);
    const auto length = starts.size();

    if (length <= maxLength)
        return legal;

    const auto byteOf = [&](std::size_t character)
    { return character < length ? starts[character] : legal.size(); };

    const auto dotByte = legal.find_last_of('.');

    // A dot within the last 11 characters is an extension worth keeping.
    if (dotByte != std::string::npos)
    {
        const auto dot = static_cast<std::size_t>(
            std::lower_bound(starts.begin(), starts.end(), dotByte)
            - starts.begin());

        if (dot > 0 && dot + 12 > length)
            return legal.substr(0, byteOf(maxLength - (length - dot)))
                   + legal.substr(dotByte);
    }

    return legal.substr(0, byteOf(maxLength));
}

namespace
{
bool isAllDigits(std::string_view text)
{
    return std::all_of(
        text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

int parseLeadingNumber(std::string_view digits)
{
    auto number = 0;
    std::from_chars(digits.data(), digits.data() + digits.size(), number);
    return number;
}
} // namespace

FilePath nonexistentChild(const FilePath& directory,
                          std::string_view prefix,
                          std::string_view suffix)
{
    auto candidate = directory / (std::string {prefix} + std::string {suffix});

    if (!somethingAt(toStdPath(candidate)))
        return candidate;

    auto base = std::string {prefix};
    auto number = 1;

    // A name that already ends in "(n)" counts on from n instead of growing
    // a second pair of brackets.
    if (Strings::trim(base).ends_with(')'))
    {
        const auto open = base.find_last_of('(');
        const auto close = base.find_last_of(')');

        if (open != std::string::npos && open > 0 && close > open
            && isAllDigits(
                std::string_view {base}.substr(open + 1, close - open - 1)))
        {
            number = parseLeadingNumber(
                std::string_view {base}.substr(open + 1, close - open - 1));
            base.erase(open);
        }
    }

    do
    {
        auto name =
            base + "(" + std::to_string(++number) + ")" + std::string {suffix};
        candidate = directory / name;
    } while (somethingAt(toStdPath(candidate)));

    return candidate;
}

FilePath nonexistentSibling(const FilePath& path)
{
    if (!somethingAt(toStdPath(path)))
        return path;

    return nonexistentChild(path.parentDirectory(), path.stem(), path.extension());
}

namespace
{
std::vector<std::string> splitWildcards(std::string_view pattern)
{
    auto wildcards = std::vector<std::string> {};

    while (!pattern.empty())
    {
        const auto separator = pattern.find_first_of(";,");

        if (auto wildcard = Strings::trim(pattern.substr(0, separator));
            !wildcard.empty())
            wildcards.push_back(Strings::toLower(wildcard));

        if (separator == std::string_view::npos)
            break;

        pattern.remove_prefix(separator + 1);
    }

    return wildcards;
}

// One UTF-8 character per element, lowercased when it is ASCII, so '?' takes
// a whole character and case is ignored.
std::vector<std::string_view> wildcardCharacters(std::string_view text)
{
    auto characters = std::vector<std::string_view> {};
    const auto starts = codePointStarts(text);

    for (auto index = std::size_t {0}; index < starts.size(); ++index)
    {
        const auto end = index + 1 < starts.size() ? starts[index + 1] : text.size();
        characters.push_back(text.substr(starts[index], end - starts[index]));
    }

    return characters;
}

// The classic greedy match with one backtrack point: on a mismatch, the last
// '*' takes one more character and the rest is tried again.
bool matchesOneWildcard(std::string_view lowerName, std::string_view wildcard)
{
    const auto name = wildcardCharacters(lowerName);
    const auto pattern = wildcardCharacters(wildcard);

    auto n = std::size_t {0};
    auto p = std::size_t {0};
    auto starAt = std::string_view::npos;
    auto resumeAt = std::size_t {0};

    while (n < name.size())
    {
        if (p < pattern.size() && pattern[p] == "*")
        {
            starAt = p++;
            resumeAt = n;
        }
        else if (p < pattern.size() && (pattern[p] == "?" || pattern[p] == name[n]))
        {
            ++p;
            ++n;
        }
        else if (starAt != std::string_view::npos)
        {
            p = starAt + 1;
            n = ++resumeAt;
        }
        else
        {
            return false;
        }
    }

    while (p < pattern.size() && pattern[p] == "*")
        ++p;

    return p == pattern.size();
}

bool matchesAnyWildcard(std::string_view name,
                        const std::vector<std::string>& wildcards)
{
    if (wildcards.empty())
        return true;

    const auto lowerName = Strings::toLower(name);

    return std::any_of(wildcards.begin(),
                       wildcards.end(),
                       [&](const std::string& wildcard)
                       { return matchesOneWildcard(lowerName, wildcard); });
}
} // namespace

bool matchesWildcard(std::string_view name, std::string_view pattern)
{
    return matchesAnyWildcard(name, splitWildcards(pattern));
}

FilePath applicationPath()
{
    auto module = Plugins::getCurrentModulePath();

    if (module.empty())
        return {};

    if constexpr (Platform::isIOS())
        return module.parentDirectory();

    if constexpr (Platform::isMac())
        if (auto bundle = enclosingBundle(module); !bundle.empty())
            return bundle;

    return module;
}

FilePath enclosingBundle(const FilePath& binary)
{
    constexpr auto bundleTail = std::string_view {"/Contents/MacOS"};

    const auto parent = binary.parentDirectory();
    const auto& text = parent.str();

    if (text.size() <= bundleTail.size()
        || !Strings::equalsCaseInsensitive(
            std::string_view {text}.substr(text.size() - bundleTail.size()),
            bundleTail))
        return {};

    return parent.parentDirectory().parentDirectory();
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
        , wildcards(splitWildcards(optionsToUse.pattern))
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
            if (!passesPattern(child))
                continue;

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

    // The pattern filters files only: a directory is always visited, so the
    // walk can still reach the matching files below it.
    bool passesPattern(const DirectoryEntry& entry) const
    {
        if (wildcards.empty() || leadsToDirectory(entry, toStdPath(entry.path)))
            return true;

        return matchesAnyWildcard(entry.path.fileName(), wildcards);
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
    std::vector<std::string> wildcards;
    std::set<std::filesystem::path> entered;
};
} // namespace

File DirectoryEntry::file() const
{
    return File {path};
}

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
