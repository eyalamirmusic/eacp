#include "LockInternal.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace eacp::IPC::detail
{
namespace
{
[[noreturn]] void throwErrno(const std::string& context)
{
    throw Error(context + ": " + std::strerror(errno));
}
} // namespace

NativeFile lockFileOpen(const FilePath& path)
{
    // O_CLOEXEC is load-bearing: an flock lives on the open file description,
    // so a child that inherited this descriptor would share the lock and keep
    // it alive after we died. eacp spawns children (Processes::run), so this
    // is a real path, not a hypothetical one.
    auto fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);

    if (fd < 0)
        throwErrno("cannot open lock file '" + path.str() + "'");

    return fd;
}

bool lockFileTryLock(NativeFile file)
{
    // flock, not fcntl: fcntl locks are owned by the process and are dropped
    // when any descriptor onto the file closes anywhere in it. flock binds to
    // this open file description instead, which is what makes two threads of
    // one process contend on the same terms as two processes.
    while (::flock((int) file, LOCK_EX | LOCK_NB) != 0)
    {
        if (errno == EWOULDBLOCK)
            return false;

        if (errno != EINTR)
            throwErrno("cannot take lock");
    }

    return true;
}

void lockFileUnlock(NativeFile file) noexcept
{
    if (file != invalidFile)
        ::flock((int) file, LOCK_UN);
}

void lockFileClose(NativeFile file) noexcept
{
    if (file != invalidFile)
        ::close((int) file);
}

} // namespace eacp::IPC::detail
