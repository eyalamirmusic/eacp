// The parts of the file helpers that only mean something on POSIX: permission
// bits and symlinks, which writeFileAtomically has to preserve across its
// rename, and a FIFO, the one readable thing whose size cannot be asked for in
// advance. The portable half is in FilesTests.cpp.

#include "Common.h"
#include <csignal>
#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <thread>

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
        std::span {reinterpret_cast<const std::uint8_t*>(contents.data()),
                   contents.size()});
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
