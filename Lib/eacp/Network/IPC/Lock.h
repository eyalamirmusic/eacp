#pragma once

#include "../Common.h"

namespace eacp::IPC
{

// Thrown when the ask itself could not be made - an unwritable directory, a
// name that resolves to nothing. Losing a lock to another holder is an ordinary
// return value, not an exception.
struct Error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// A named lock scoped to this user's processes on this machine, held only via
// ScopedLock. The kernel owns the release, so a crash frees it; the lock file
// is never deleted. Two guards conflict across threads as across processes.
class Lock
{
public:
    // Establishes the lock without taking it. name is folded to a file under
    // FilePath::appDataDirectory(), so pick one already distinct on its own.
    // Throws IPC::Error when the name cannot be backed by a file at all.
    explicit Lock(std::string_view name);

    ~Lock();

    // Neither copyable nor movable: a live ScopedLock refers back to its Lock.
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    Lock(Lock&&) = delete;
    Lock& operator=(Lock&&) = delete;

private:
    friend class ScopedLock;

    bool tryAcquire();
    bool tryAcquire(Time::MS timeout);
    void release();

    struct Impl;
    OwningPointer<Impl> impl;
};

// Holds a Lock for as long as it is in scope - if it won it. Construction
// never fails and never blocks indefinitely, so always ask isLocked() before
// entering the critical section.
class ScopedLock
{
public:
    // Takes the lock if it is free, and gives up immediately if it is not.
    explicit ScopedLock(Lock& lockToUse);

    // Polls until timeout elapses, because neither platform primitive offers a
    // timed wait. No wait-forever overload, deliberately.
    ScopedLock(Lock& lockToUse, Time::MS timeout);

    ~ScopedLock();

    ScopedLock(const ScopedLock&) = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;
    ScopedLock(ScopedLock&&) = delete;
    ScopedLock& operator=(ScopedLock&&) = delete;

    [[nodiscard]] bool isLocked() const { return locked; }
    explicit operator bool() const { return locked; }

private:
    Lock& lock;
    bool locked = false;
};

} // namespace eacp::IPC
