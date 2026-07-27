#include "AllocationCount.h"
#include "Common.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <new>
#include <thread>

#if !defined(_WIN32)
#include <csignal>
#include <sys/stat.h>
#endif

#include <filesystem>
#include <fstream>
#include <span>

using namespace nano;
using eacp::File;
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
        std::span {reinterpret_cast<const std::uint8_t*>(contents.data()),
                   contents.size()});
}

std::size_t entryCount(const std::filesystem::path& dir)
{
    auto count = std::size_t {0};

    for (const auto& entry: std::filesystem::directory_iterator {dir})
    {
        (void) entry;
        ++count;
    }

    return count;
}
} // namespace

auto tAtomicCreatesFile = test("Files/atomicCreatesFile") = []
{
    auto dir = scratchDirectory("create");
    auto path = dir / "new.txt";

    writeAtomically(path, "hello");

    check(read(path) == "hello");

    std::filesystem::remove_all(dir);
};

auto tAtomicReplacesWholeFile = test("Files/atomicReplacesWholeFile") = []
{
    auto dir = scratchDirectory("replace");
    auto path = dir / "existing.txt";

    write(path, "a much longer previous version of the file");
    writeAtomically(path, "short");

    // A truncating write that stopped early would leave the old tail behind.
    check(read(path) == "short");

    std::filesystem::remove_all(dir);
};

auto tAtomicLeavesNoTemporaries = test("Files/atomicLeavesNoTemporaries") = []
{
    auto dir = scratchDirectory("no-litter");
    auto path = dir / "doc.txt";

    writeAtomically(path, "one");
    writeAtomically(path, "two");
    writeAtomically(path, "three");

    // The temporary is renamed onto the target rather than left beside it, so
    // repeated saves must not accumulate files in the directory.
    check(entryCount(dir) == 1);
    check(read(path) == "three");

    std::filesystem::remove_all(dir);
};

auto tAtomicCreatesParentDirectories =
    test("Files/atomicCreatesParentDirectories") = []
{
    auto dir = scratchDirectory("parents");
    auto path = dir / "a" / "b" / "c.txt";

    writeAtomically(path, "nested");

    check(read(path) == "nested");

    std::filesystem::remove_all(dir);
};

auto tAtomicWritesEmpty = test("Files/atomicWritesEmpty") = []
{
    auto dir = scratchDirectory("empty");
    auto path = dir / "doc.txt";

    write(path, "not empty yet");
    writeAtomically(path, "");

    check(File {path}.exists());
    check(File {path}.size() == 0);

    std::filesystem::remove_all(dir);
};

auto tAtomicThrowsOnUnwritableTarget =
    test("Files/atomicThrowsOnUnwritableTarget") = []
{
    auto dir = scratchDirectory("unwritable");

    // The target is a directory, so the rename can never succeed.
    std::filesystem::create_directories(dir / "target");

    auto threw = false;

    try
    {
        writeAtomically(dir / "target", "nope");
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }

    check(threw);

    // And the failed attempt cleans up after itself.
    check(entryCount(dir) == 1);

    std::filesystem::remove_all(dir);
};

// Permission bits and symlinks are POSIX concepts; Windows has neither in the
// form these assert on.
#ifndef _WIN32

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

#endif

auto tModificationTimeMoves = test("File/modificationTimeMoves") = []
{
    auto dir = scratchDirectory("mtime");
    auto path = dir / "doc.txt";

    write(path, "first");
    const auto first = File {path}.modificationTime();

    check(first != 0);
    check(File {path}.modificationTime() == first);

    // Filesystem timestamp granularity is coarse enough on some filesystems
    // that two writes in the same millisecond share a stamp, so this stamps the
    // file explicitly rather than racing the clock.
    std::filesystem::last_write_time(
        path, std::filesystem::last_write_time(path) + std::chrono::seconds {2});

    check(File {path}.modificationTime() != first);

    std::filesystem::remove_all(dir);
};

auto tModificationTimeMissing = test("File/modificationTimeMissing") = []
{
    auto dir = scratchDirectory("mtime-missing");

    check(File {dir / "nothing-here.txt"}.modificationTime() == 0);

    std::filesystem::remove_all(dir);
};

// --- reading ----------------------------------------------------------------
//
// Everything above uses readFile as a helper for checking what a write produced,
// so none of it asserts anything about the read.

// A doubling buffer plus a copy out measured 4.00x the file; reading into one
// sized allocation is 1.0x. Anything under 2x separates them with room to spare.
auto tReadAllocatesAboutTheFileSize =
    test("Files/readAllocatesAboutTheFileSize") = []
{
    const auto dir = scratchDirectory("read-cost");
    const auto path = dir / "big.txt";

    // Large enough that a doubling buffer reallocates many times, so the
    // difference is structural rather than a fixed overhead.
    const auto size = std::size_t {2 * 1024 * 1024};
    write(path, std::string(size, 'x'));

    auto counter = AllocationCount {};
    const auto contents = read(path);
    const auto bytes = counter.bytes();

    check(contents.size() == size);
    check(bytes < size * 2);
};

auto tReadsAnEmptyFile = test("Files/readsAnEmptyFile") = []
{
    const auto dir = scratchDirectory("read-empty");
    const auto path = dir / "empty.txt";

    write(path, "");

    check(read(path).empty());
};

auto tReadsAMissingFileAsEmpty = test("Files/readsAMissingFileAsEmpty") = []
{
    const auto dir = scratchDirectory("read-missing");

    check(read(dir / "does-not-exist.txt").empty());
};

auto tReadsWithoutATrailingNewline =
    test("Files/readsAFileWithNoTrailingNewline") = []
{
    const auto dir = scratchDirectory("read-no-newline");
    const auto path = dir / "text.txt";

    write(path, "one\ntwo");

    check(read(path) == "one\ntwo");
};

// A file is a length and some bytes, not a C string.
auto tReadsEmbeddedNulBytes = test("Files/readsEmbeddedNulBytes") = []
{
    const auto dir = scratchDirectory("read-nuls");
    const auto path = dir / "binary.bin";

    const auto contents = std::string {"before\0after\0\0end", 17};
    write(path, contents);

    check(read(path) == contents);
    check(read(path).size() == 17);
};

#if !defined(_WIN32)
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
#endif
