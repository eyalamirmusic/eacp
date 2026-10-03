// The parts of the file helpers that only mean something on POSIX: permission
// bits and symlinks, which writeFileAtomically has to preserve across its
// rename, and a FIFO, the one readable thing whose size cannot be asked for in
// advance. The portable half is in FilesTests.cpp.

#include "Common.h"
#include <eacp/Core/Utils/StdPath.h>
#include <csignal>
#include <unistd.h>
#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace nano;
using eacp::FilePath;

namespace
{
std::filesystem::path scratchDirectory(const std::string& name)
{
    auto dir = std::filesystem::temp_directory_path() / ("eacp-files-" + name);

    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    return dir;
}

void write(const std::filesystem::path& path, const std::string& contents)
{
    auto out = std::ofstream {path, std::ios::binary | std::ios::trunc};
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

std::string read(const std::filesystem::path& path)
{
    return eacp::Files::readFile(FilePath {path});
}

void writeAtomically(const std::filesystem::path& path, std::string_view contents)
{
    eacp::Files::writeFileAtomically(
        FilePath {path},
        eacp::Span {reinterpret_cast<const std::uint8_t*>(contents.data()),
                    (int) contents.size()});
}
} // namespace

auto tAtomicKeepsPermissions = test("Files/atomicKeepsPermissions") = []
{
    auto dir = scratchDirectory("permissions");
    auto path = dir / "script.sh";

    write(path, "#!/bin/sh\necho old\n");

    const auto executable = std::filesystem::perms::owner_all
                            | std::filesystem::perms::group_read
                            | std::filesystem::perms::group_exec;

    std::filesystem::permissions(path, executable);

    writeAtomically(path, "#!/bin/sh\necho new\n");

    // Without the copy, the renamed-in file arrives with the process umask and
    // the script stops being runnable.
    check(std::filesystem::status(path).permissions() == executable);

    std::filesystem::remove_all(dir);
};

auto tAtomicFollowsSymlinks = test("Files/atomicFollowsSymlinks") = []
{
    auto dir = scratchDirectory("symlink");
    auto real = dir / "real.txt";
    auto link = dir / "link.txt";

    write(real, "original");
    std::filesystem::create_symlink(real, link);

    writeAtomically(link, "through the link");

    // The link must still be a link, pointing at a file that now has the new
    // contents -- renaming over it would have made it a regular file and left
    // the real one stale.
    check(std::filesystem::is_symlink(link));
    check(read(real) == "through the link");

    std::filesystem::remove_all(dir);
};

// A FIFO has no size to report in advance, so a reader that asks for one and
// reads exactly that many bytes returns nothing at all. On macOS file_size
// throws here rather than answering zero.
auto tReadsAStreamWithNoKnownSize = test("Files/readsAStreamWhoseSizeIsUnknown") = []
{
    const auto dir = scratchDirectory("read-fifo");
    const auto path = dir / "pipe";

    if (::mkfifo(path.c_str(), 0600) != 0)
        return;

    auto sizeError = std::error_code {};
    const auto reported = std::filesystem::file_size(path, sizeError);

    check(sizeError || reported == 0);

    // Inside a pipe's buffer, so the writer never waits on the reader.
    const auto contents = std::string(16 * 1024, 'p');

    // A reader that gives up without draining closes its end, and the write
    // below then raises SIGPIPE — killing the binary before any test can
    // report, so a broken readFile looks like the suite vanishing rather than
    // like one assertion failing.
    struct IgnoreSigPipe
    {
        IgnoreSigPipe() { previous = std::signal(SIGPIPE, SIG_IGN); }
        ~IgnoreSigPipe() { std::signal(SIGPIPE, previous); }

        void (*previous)(int) = nullptr;
    } ignoreSigPipe;

    // Another thread, because opening either end of a FIFO blocks until the
    // other is open. jthread so that a readFile which throws does not destroy it
    // while joinable and terminate the whole suite.
    auto writer =
        std::jthread {[&]
                      {
                          auto out = std::ofstream {path, std::ios::binary};
                          out.write(contents.data(),
                                    static_cast<std::streamsize>(contents.size()));
                      }};

    const auto read = eacp::Files::readFile(FilePath {path});

    check(read.size() == contents.size());
    check(read == contents);
};

namespace
{
using eacp::Files::DirectoryEntry;
using eacp::Files::DirectoryOptions;
using eacp::Files::EntryKind;
using eacp::Files::Symlinks;
using eacp::Files::Visit;

std::vector<std::string> relativeNames(const std::filesystem::path& root,
                                       const eacp::Vector<DirectoryEntry>& entries)
{
    auto names = std::vector<std::string> {};

    for (const auto& entry: entries)
        names.push_back(
            eacp::toStdPath(entry.path).lexically_relative(root).generic_string());

    return names;
}

// a/x.txt, a/up -> a (a cycle), file-link -> a/x.txt, dir-link -> a.
std::filesystem::path linkedTree(const std::string& name)
{
    auto dir = scratchDirectory(name);

    std::filesystem::create_directories(dir / "a");
    write(dir / "a" / "x.txt", "x");
    std::filesystem::create_directory_symlink(dir / "a", dir / "a" / "up");
    std::filesystem::create_symlink(dir / "a" / "x.txt", dir / "file-link");
    std::filesystem::create_directory_symlink(dir / "a", dir / "dir-link");

    return dir;
}

DirectoryOptions recursiveWith(Symlinks symlinks)
{
    auto options = DirectoryOptions {};
    options.recursive = true;
    options.symlinks = symlinks;
    return options;
}
} // namespace

auto tSymlinksSkipped =
    test("Files/symlinksAreReportedAsThemselvesAndNotEntered") = []
{
    auto dir = linkedTree("list-symlink-skip");

    auto entries =
        eacp::Files::listDirectory(FilePath {dir}, recursiveWith(Symlinks::skip));

    check(relativeNames(dir, entries)
          == std::vector<std::string> {
              "a", "a/up", "a/x.txt", "dir-link", "file-link"});
    check(entries[1].kind == EntryKind::symlink);
    check(entries[3].kind == EntryKind::symlink);
    check(entries[4].kind == EntryKind::symlink);
    check(entries[4].file().isRegularFile());

    auto files =
        eacp::Files::listFiles(FilePath {dir}, recursiveWith(Symlinks::skip));
    check(files.size() == 1);

    std::filesystem::remove_all(dir);
};

auto tSymlinksFollowed = test("Files/followEntersEachDirectoryOnce") = []
{
    auto dir = linkedTree("list-symlink-follow");

    auto entries =
        eacp::Files::listDirectory(FilePath {dir}, recursiveWith(Symlinks::follow));

    check(relativeNames(dir, entries)
          == std::vector<std::string> {
              "a", "a/up", "a/x.txt", "dir-link", "file-link"});

    auto files =
        eacp::Files::listFiles(FilePath {dir}, recursiveWith(Symlinks::follow));
    check(files.size() == 2);

    std::filesystem::remove_all(dir);
};

auto tUnreadableDirectory = test("Files/anUnreadableDirectoryReachesOnError") = []
{
    if (geteuid() == 0)
        return;

    auto dir = scratchDirectory("list-unreadable");
    std::filesystem::create_directories(dir / "locked");
    write(dir / "locked" / "secret.txt", "s");
    write(dir / "open.txt", "o");
    std::filesystem::create_directories(dir / "zed");

    chmod((dir / "locked").c_str(), 0);

    auto options = DirectoryOptions {};
    options.recursive = true;
    auto reported = std::vector<FilePath> {};

    options.onError = [&](const eacp::Files::TraversalError& error)
    {
        reported.push_back(error.path);
        return Visit::next;
    };

    auto entries = eacp::Files::listDirectory(FilePath {dir}, options);

    check(relativeNames(dir, entries)
          == std::vector<std::string> {"locked", "open.txt", "zed"});
    check(reported.size() == 1);
    check(reported.front() == FilePath {dir / "locked"});

    options.onError = [&](const eacp::Files::TraversalError&)
    { return Visit::stop; };

    check(relativeNames(dir, eacp::Files::listDirectory(FilePath {dir}, options))
          == std::vector<std::string> {"locked"});

    chmod((dir / "locked").c_str(), 0755);
    std::filesystem::remove_all(dir);
};

#ifdef __APPLE__
auto tFinderHidden = test("Files/aFinderHiddenFileIsHiddenWithoutADot") = []
{
    auto dir = scratchDirectory("list-finder-hidden");
    write(dir / "plain.txt", "p");
    write(dir / "flagged.txt", "f");

    check(chflags((dir / "flagged.txt").c_str(), UF_HIDDEN) == 0);

    check(relativeNames(dir, eacp::Files::listDirectory(FilePath {dir}))
          == std::vector<std::string> {"plain.txt"});

    auto options = DirectoryOptions {};
    options.includeHidden = true;
    auto all = eacp::Files::listDirectory(FilePath {dir}, options);

    check(relativeNames(dir, all)
          == std::vector<std::string> {"flagged.txt", "plain.txt"});
    check(all[0].isHidden);
    check(!all[1].isHidden);

    std::filesystem::remove_all(dir);
};
#endif
