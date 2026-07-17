#include "LockInternal.h"
#include "Names.h"

#include <eacp/Core/Utils/StdPath.h>

#include <mutex>
#include <system_error>

namespace eacp::IPC
{
namespace
{
constexpr auto pollInterval = Time::MS {25};

FilePath lockDirectory()
{
    auto appData = FilePath::appDataDirectory();

    if (appData.empty())
        throw Error("cannot resolve the app data directory");

    auto directory = appData / "eacp" / "Locks";
    auto failure = std::error_code {};
    std::filesystem::create_directories(toStdPath(directory), failure);

    if (failure)
        throw Error("cannot create '" + directory.str() + "': " + failure.message());

    return directory;
}

FilePath lockFilePath(std::string_view name)
{
    if (name.empty())
        throw Error("lock name is empty");

    return lockDirectory() / (detail::foldToFileName(name) + ".lock");
}
} // namespace

// The handle lives here for the Lock's whole life: opened once, then locked
// and unlocked by each guard in turn. held is what keeps the platforms
// honest - flock() re-locking a handle it already owns quietly succeeds while
// LockFileEx() reports a violation, so neither is asked twice.
struct Lock::Impl
{
    ~Impl() { detail::lockFileClose(file); }

    std::mutex mutex;
    detail::NativeFile file = detail::invalidFile;
    bool held = false;
};

Lock::Lock(std::string_view name)
{
    impl.create();
    impl->file = detail::lockFileOpen(lockFilePath(name));
}

Lock::~Lock() = default;

bool Lock::tryAcquire()
{
    auto guard = std::lock_guard {impl->mutex};

    if (impl->held)
        return false;

    if (!detail::lockFileTryLock(impl->file))
        return false;

    impl->held = true;
    return true;
}

bool Lock::tryAcquire(Time::MS timeout)
{
    auto deadline = Time::Deadline {timeout};

    for (;;)
    {
        if (tryAcquire())
            return true;

        if (deadline.expired())
            return false;

        auto remaining = deadline.remaining();
        Time::sleep(remaining < pollInterval ? remaining : pollInterval);
    }
}

void Lock::release()
{
    auto guard = std::lock_guard {impl->mutex};

    if (!impl->held)
        return;

    detail::lockFileUnlock(impl->file);
    impl->held = false;
}

ScopedLock::ScopedLock(Lock& lockToUse)
    : lock(lockToUse)
{
    locked = lock.tryAcquire();
}

ScopedLock::ScopedLock(Lock& lockToUse, Time::MS timeout)
    : lock(lockToUse)
{
    locked = lock.tryAcquire(timeout);
}

ScopedLock::~ScopedLock()
{
    if (locked)
        lock.release();
}

} // namespace eacp::IPC
