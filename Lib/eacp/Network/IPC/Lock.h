#pragma once

#include "../Common.h"

namespace eacp::IPC
{

// Every failure surfaces as this one type, with a message ready to log. Note
// what is not an error: losing a lock to another holder is an ordinary return
// value, not an exception. This is thrown when the ask itself could not be
// made - an unwritable directory, a name that resolves to nothing.
struct Error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// A named lock, shared by every process this user runs on this machine.
//
// Constructing one establishes the name but takes nothing: a Lock is the
// handle, and ScopedLock is the only way to hold it. Declare the Lock where it
// lives and guard the critical sections with ScopedLock, the way a std::mutex
// pairs with std::scoped_lock.
//
// The kernel owns the release. Dropping the guard releases the lock, and so
// does an abnormal exit - a crash, a SIGKILL, a debugger stop - because the
// underlying file lock dies with the file handle. A holder that vanishes never
// strands the lock, which is what makes this safe to build restart-anything on
// and why a hand-rolled pid file is not. The lock file itself is never
// deleted: unlinking it would let the next process lock a different inode and
// believe it had won.
//
// Threads are not a special case. Two guards conflict whether they come from
// two processes or two threads of one, and a Lock is safe to share between
// threads. There is no recursion: a second guard over a Lock already held -
// by this object, this process or any other - is told no.
//
// Scope is the user, not the machine. Another user running the same app gets
// their own lock, which is what a per-user app wants (fast user switching
// should not make the second login lose a coin toss).
class Lock
{
public:
    // Establishes the lock named name without taking it. Throws IPC::Error
    // when the name cannot be backed by a file at all.
    //
    // name identifies the lock among this user's processes and is mapped to a
    // file under FilePath::appDataDirectory(); characters that a filename
    // cannot carry are folded, so pick a name that is already distinct on its
    // own - a bundle id rather than a word.
    explicit Lock(std::string_view name);

    ~Lock();

    // Neither copyable nor movable: a live ScopedLock refers back to its Lock,
    // and moving out from under it would leave that reference dangling.
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

    // Retries until timeout elapses, for callers content to queue rather than
    // give up. Polls, because neither platform primitive offers a timed wait.
    // There is deliberately no wait-forever overload: an unbounded wait on a
    // lock another process may never drop is a hang with extra steps.
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
