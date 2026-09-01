#include "AllocationCount.h"
#include "Common.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <new>
#include <filesystem>
#include <fstream>
#include <span>
#include <thread>

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
