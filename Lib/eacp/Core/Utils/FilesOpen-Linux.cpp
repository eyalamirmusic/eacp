#include "Files.h"
#include "FilesPlatform.h"

#include "../Process/Process.h"

#include <thread>

namespace eacp::Detail
{
namespace
{
// Spawned with the path as an argument, never system(): a shell would read
// the metacharacters a file name may contain as its own.
//
// Not a detached Process: those are never reaped, so every call would leave
// a zombie until the app quits. A thread of its own waits for xdg-open
// instead, which usually hands off to the desktop and exits at once, but may
// also run the handler itself and wait for it; either way the caller does
// not.
bool launchXdgOpen(const FilePath& target)
{
    auto options = Processes::ProcessOptions {};
    options.executable = "xdg-open";
    options.arguments.add(target.str());
    options.captureOutput = false;

    auto opener = Processes::Process {std::move(options)};

    // launched() also answers true when posix_spawn found no xdg-open to run.
    if (opener.id() <= 0)
        return false;

    std::thread([opener = std::move(opener)]() mutable { opener.wait(); }).detach();

    return true;
}
} // namespace

bool openWithSystem(const FilePath& target)
{
    return launchXdgOpen(target);
}

// No portable way to select an item: the FileManager1 D-Bus interface is
// Nautilus's and Dolphin's but not every desktop's, so the folder holding it
// is opened instead.
bool revealInFileManager(const FilePath& target)
{
    return launchXdgOpen(Files::revealFallbackDirectory(target));
}
} // namespace eacp::Detail
