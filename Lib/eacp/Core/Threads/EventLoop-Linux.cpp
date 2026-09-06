#include "EventLoop-Linux.h"
#include "ThreadUtils-Linux.h"
#include "../Utils/Singleton.h"

#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <unistd.h>

namespace eacp::Threads
{

struct PipeWaker
{
    PipeWaker()
    {
        int fds[2];
        if (::pipe(fds) != 0)
            return;

        readFd = fds[0];
        writeFd = fds[1];

        ::fcntl(readFd, F_SETFL, O_NONBLOCK);
        ::fcntl(writeFd, F_SETFL, O_NONBLOCK);
        ::fcntl(readFd, F_SETFD, FD_CLOEXEC);
        ::fcntl(writeFd, F_SETFD, FD_CLOEXEC);
    }

    ~PipeWaker()
    {
        if (readFd >= 0)
            ::close(readFd);
        if (writeFd >= 0)
            ::close(writeFd);
    }

    void wake()
    {
        char b = 1;
        auto r = ::write(writeFd, &b, 1);
        (void) r;
    }

    void drain()
    {
        char buf[64];
        while (::read(readFd, buf, sizeof(buf)) > 0)
        {
        }
    }

    int readFd = -1;
    int writeFd = -1;
};

struct LoopSource
{
    int fd = -1;
    short events = 0;
    Callback callback;
    Callback prepare;
};

struct LoopState
{
    PipeWaker waker;
    std::mutex mutex;
    Vector<Callback> queue;
    std::atomic<bool> running {false};

    std::mutex sourceMutex;
    Vector<LoopSource> sources;
};

static LoopState& getLoop()
{
    return Singleton::get<LoopState>();
}

namespace
{
void drainPending(LoopState& loop)
{
    auto pending = Vector<Callback>();
    {
        auto lock = std::lock_guard(loop.mutex);
        pending = std::move(loop.queue);
    }
    for (auto& cb: pending)
        cb();
}

// Runs every source's prepare callback, before the poll set is built rather
// than after, so a prepare that registers or drops a source is reflected in
// the very wait it precedes. Copied out from under the lock for the same
// reason dispatchReadySources does it: a prepare may touch the source list,
// including its own entry.
void runSourcePrepares(LoopState& loop)
{
    auto prepares = Vector<Callback> {};

    {
        auto lock = std::lock_guard(loop.sourceMutex);

        for (const auto& source: loop.sources)
            if (source.prepare)
                prepares.add(source.prepare);
    }

    for (auto& prepare: prepares)
        prepare();
}

// The waker first, then every registered source. Rebuilt before each wait
// rather than cached, because a source callback is allowed to add or remove
// sources — including its own — while the set is being dispatched.
Vector<pollfd> buildPollSet(LoopState& loop)
{
    auto fds = Vector<pollfd> {};
    fds.add(pollfd {loop.waker.readFd, POLLIN, 0});

    auto lock = std::lock_guard(loop.sourceMutex);

    for (const auto& source: loop.sources)
        fds.add(pollfd {source.fd, source.events, 0});

    return fds;
}

// Runs the callback of every source poll() reported on, looking each one up by
// descriptor so a source removed by an earlier callback in the same round is
// simply not found. The callback is copied out before the lock is released, so
// a source that removes itself is still alive for the duration of the call.
void dispatchReadySources(LoopState& loop, const Vector<pollfd>& fds)
{
    for (auto i = 1; i < fds.size(); ++i)
    {
        if (fds[i].revents == 0)
            continue;

        auto callback = Callback {};

        {
            auto lock = std::lock_guard(loop.sourceMutex);

            for (const auto& source: loop.sources)
                if (source.fd == fds[i].fd)
                    callback = source.callback;
        }

        if (callback)
            callback();
    }
}

int waitForLoopActivity(Vector<pollfd>& fds, int timeoutMs)
{
    return ::poll(fds.data(), (nfds_t) fds.size(), timeoutMs);
}
} // namespace

void EventLoop::run()
{
    initMainThread();

    auto& loop = getLoop();
    loop.running = true;

    while (loop.running)
    {
        runSourcePrepares(loop);

        auto fds = buildPollSet(loop);
        auto r = waitForLoopActivity(fds, -1);

        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }

        loop.waker.drain();
        dispatchReadySources(loop, fds);
        drainPending(loop);
    }
}

bool EventLoop::runFor(Time::MS timeout)
{
    initMainThread();

    auto& loop = getLoop();
    loop.running = true;

    auto deadline = Time::Deadline {timeout};
    auto timedOut = false;

    while (loop.running)
    {
        if (deadline.expired())
        {
            timedOut = true;
            break;
        }

        auto remaining = deadline.remaining().count;

        runSourcePrepares(loop);

        auto fds = buildPollSet(loop);
        auto r = waitForLoopActivity(fds, (int) remaining);

        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        if (r == 0)
        {
            timedOut = true;
            break;
        }

        loop.waker.drain();
        dispatchReadySources(loop, fds);
        drainPending(loop);
    }

    return !timedOut;
}

void EventLoop::quit()
{
    auto& loop = getLoop();
    loop.running = false;
    loop.waker.wake();
}

void EventLoop::call(Callback func)
{
    auto& loop = getLoop();
    {
        auto lock = std::lock_guard(loop.mutex);
        loop.queue.add(std::move(func));
    }
    loop.waker.wake();
}

void addLoopSource(int fd, short events, Callback callback, Callback prepare)
{
    auto& loop = getLoop();

    {
        auto lock = std::lock_guard(loop.sourceMutex);

        loop.sources.removeIndexesMatching([fd](const LoopSource& source)
                                           { return source.fd == fd; });

        loop.sources.add(
            LoopSource {fd, events, std::move(callback), std::move(prepare)});
    }

    // The pump may already be blocked in poll() over a set this descriptor is
    // not in yet, so nothing else would make it rebuild.
    loop.waker.wake();
}

void addLoopSource(int fd, short events, Callback callback)
{
    addLoopSource(fd, events, std::move(callback), Callback {});
}

void removeLoopSource(int fd)
{
    auto& loop = getLoop();

    {
        auto lock = std::lock_guard(loop.sourceMutex);

        loop.sources.removeIndexesMatching([fd](const LoopSource& source)
                                           { return source.fd == fd; });
    }

    loop.waker.wake();
}

void scheduleStartup(const Callback& func)
{
    callAsync(func);
}

bool isEventLoopRunning()
{
    return getLoop().running.load();
}

// The Linux loop is per-copy (no process-global pump to reach into yet);
// wire this up alongside a Linux plugin host when one exists.
void stopProcessRootLoop() {}

} // namespace eacp::Threads
