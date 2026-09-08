#include "EventLoop-Linux.h"
#include "ThreadUtils-Linux.h"
#include "../Utils/Singleton.h"

#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <sys/epoll.h>
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

namespace
{
constexpr auto maxReadySourcesPerPump = 32;

uint32_t epollEventsFromPollEvents(short events)
{
    auto result = uint32_t {};

    if ((events & POLLIN) != 0)
        result |= EPOLLIN;
    if ((events & POLLOUT) != 0)
        result |= EPOLLOUT;
    if ((events & POLLPRI) != 0)
        result |= EPOLLPRI;

    return result;
}

void watchLoopFd(int epollFd, int fd, short events)
{
    auto event = epoll_event {};
    event.events = epollEventsFromPollEvents(events);
    event.data.fd = fd;

    if (::epoll_ctl(epollFd, EPOLL_CTL_MOD, fd, &event) != 0)
        ::epoll_ctl(epollFd, EPOLL_CTL_ADD, fd, &event);
}

void unwatchLoopFd(int epollFd, int fd)
{
    ::epoll_ctl(epollFd, EPOLL_CTL_DEL, fd, nullptr);
}
} // namespace

// One epoll instance holds the waker and every source, so a host watches a
// single descriptor for this copy while sources come and go behind it.
struct LoopState
{
    LoopState()
        : epollFd(::epoll_create1(EPOLL_CLOEXEC))
    {
        watchLoopFd(epollFd, waker.readFd, POLLIN);
    }

    ~LoopState()
    {
        if (epollFd >= 0)
            ::close(epollFd);
    }

    LoopState(const LoopState&) = delete;
    LoopState& operator=(const LoopState&) = delete;

    PipeWaker waker;
    int epollFd = -1;

    std::mutex mutex;
    Vector<Callback> queue;

    std::atomic<bool> running {false};
    std::atomic<bool> hosted {false};
    std::atomic<int> pumpDepth {0};

    std::mutex sourceMutex;
    Vector<LoopSource> sources;
};

static LoopState& getLoop()
{
    return Singleton::get<LoopState>();
}

namespace
{
struct PumpScope
{
    explicit PumpScope(LoopState& loopToUse)
        : loop(loopToUse)
    {
        ++loop.pumpDepth;
    }

    ~PumpScope() { --loop.pumpDepth; }

    PumpScope(const PumpScope&) = delete;
    PumpScope& operator=(const PumpScope&) = delete;

    LoopState& loop;
};

enum class WaitResult
{
    Ready,
    TimedOut,
    Failed
};

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

// Callbacks are copied out from under the lock throughout: one is allowed to
// add or remove sources, including its own entry.
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

// By descriptor, so a source removed earlier in the round is not found.
void dispatchReadySources(LoopState& loop)
{
    auto ready = Array<epoll_event, maxReadySourcesPerPump> {};
    auto count = ::epoll_wait(loop.epollFd, ready.data(), ready.size(), 0);

    for (auto i = 0; i < count; ++i)
    {
        auto fd = ready[i].data.fd;

        if (fd == loop.waker.readFd)
            continue;

        auto callback = Callback {};

        {
            auto lock = std::lock_guard(loop.sourceMutex);

            for (const auto& source: loop.sources)
                if (source.fd == fd)
                    callback = source.callback;
        }

        if (callback)
            callback();
    }
}

// The prepares end the round rather than start it, so they are equally the
// prepares of the wait a standalone loop is about to enter and the flush a
// hosted copy needs before returning to a host that will not wait for us.
void pumpLoopOnce(LoopState& loop)
{
    loop.waker.drain();
    dispatchReadySources(loop);
    drainPending(loop);
    runSourcePrepares(loop);
}

WaitResult waitForLoopActivity(const LoopState& loop, int timeoutMs)
{
    auto fds = pollfd {loop.epollFd, POLLIN, 0};
    auto r = ::poll(&fds, 1, timeoutMs);

    if (r > 0)
        return WaitResult::Ready;

    if (r == 0)
        return WaitResult::TimedOut;

    return errno == EINTR ? WaitResult::Ready : WaitResult::Failed;
}
} // namespace

int getEventLoopFd()
{
    return getLoop().epollFd;
}

void pumpEventLoop()
{
    auto& loop = getLoop();

    if (loop.pumpDepth.load() > 0)
        return;

    auto scope = PumpScope {loop};
    pumpLoopOnce(loop);
}

void EventLoop::run()
{
    initMainThread();

    auto& loop = getLoop();
    loop.running = true;

    auto scope = PumpScope {loop};

    while (loop.running)
    {
        pumpLoopOnce(loop);

        if (!loop.running)
            break;

        if (waitForLoopActivity(loop, -1) == WaitResult::Failed)
            break;
    }
}

bool EventLoop::runFor(Time::MS timeout)
{
    initMainThread();

    auto& loop = getLoop();
    loop.running = true;

    auto scope = PumpScope {loop};
    auto deadline = Time::Deadline {timeout};

    while (loop.running)
    {
        pumpLoopOnce(loop);

        if (!loop.running)
            break;

        if (deadline.expired())
            return false;

        auto wait = waitForLoopActivity(loop, (int) deadline.remaining().count);

        if (wait == WaitResult::TimedOut)
            return false;

        if (wait == WaitResult::Failed)
            break;
    }

    return true;
}

void EventLoop::quit()
{
    auto& loop = getLoop();
    loop.running = false;
    loop.waker.wake();
}

void stopEventLoop()
{
    getEventLoop().quit();
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

        watchLoopFd(loop.epollFd, fd, events);
    }

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

        unwatchLoopFd(loop.epollFd, fd);

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
    auto& loop = getLoop();
    return loop.running.load() || loop.hosted.load();
}

void attachCurrentThreadAsMain()
{
    initMainThread();

    auto& loop = getLoop();
    loop.hosted = true;
    loop.waker.wake();
}

// The Linux loop is per-copy (no process-global pump to reach into yet);
// wire this up alongside a Linux plugin host when one exists.
void stopProcessRootLoop() {}

} // namespace eacp::Threads
