#include "EventLoop-Linux.h"
#include "ThreadUtils-Linux.h"
#include "../Platform/Platform.h"
#include "../Utils/Environment.h"
#include "../Utils/Singleton.h"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
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

// What the copy running the process's root loop offers every other copy in
// the process: add a hosted copy's loop descriptor as a source of the root's,
// remove it again, and stop the root loop. Function pointers and a void* —
// nothing with C++ layout crosses an image boundary. The address of one of
// these rides the environment (EACP_ROOT_LOOP_BRIDGE), which is the same
// cross-copy channel EACP_ROOT_LOOP_THREAD is on Windows.
using RootLoopPump = void (*)(void*);

struct RootLoopBridge
{
    unsigned magic = 0;
    unsigned size = 0;
    void (*attach)(int fd, RootLoopPump pump, void* context) = nullptr;
    void (*detach)(int fd) = nullptr;
    void (*stop)() = nullptr;
};

// A hosted copy's loop as the root copy holds it. Detach clears `pump`, so a
// callback already copied out of the source list for this round does nothing
// instead of jumping into an image that has since been unmapped.
struct RootLoopGuest
{
    int fd = -1;
    void* context = nullptr;
    std::atomic<RootLoopPump> pump {nullptr};
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

constexpr auto rootLoopMagic = 0x45414350u;
constexpr auto rootLoopMarkerName = "EACP_ROOT_LOOP";
constexpr auto rootLoopBridgeName = "EACP_ROOT_LOOP_BRIDGE";

void rootLoopAttach(int fd, RootLoopPump pump, void* context);
void rootLoopDetach(int fd);
void rootLoopStop();

// Constant-initialised and trivially destructible, so it is readable for as
// long as this image is mapped and registers nothing for atexit.
RootLoopBridge rootLoopBridge {rootLoopMagic,
                               (unsigned) sizeof(RootLoopBridge),
                               rootLoopAttach,
                               rootLoopDetach,
                               rootLoopStop};

std::string rootLoopBridgeAdvertisement()
{
    return std::to_string((long long) ::getpid()) + ':'
           + std::to_string(reinterpret_cast<uintptr_t>(&rootLoopBridge));
}

// The bridge another copy advertised, or null when no eacp copy runs the
// process's root loop. The pid is part of the advertisement because a child
// process inherits the environment without the address space that gave it
// meaning.
RootLoopBridge* findRootLoopBridge()
{
    if (getEnvValue(rootLoopMarkerName) != "1")
        return nullptr;

    const auto advertised = getEnvValue(rootLoopBridgeName);
    const auto separator = advertised.find(':');

    if (separator == std::string::npos)
        return nullptr;

    if (std::strtoll(advertised.c_str(), nullptr, 10) != (long long) ::getpid())
        return nullptr;

    const auto address =
        std::strtoull(advertised.c_str() + separator + 1, nullptr, 10);

    auto* bridge = reinterpret_cast<RootLoopBridge*>((uintptr_t) address);

    if (bridge == nullptr || bridge->magic != rootLoopMagic
        || bridge->size != sizeof(RootLoopBridge))
        return nullptr;

    return bridge;
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

    // Before the descriptor closes, and before the image this copy lives in
    // can be unmapped: the root copy holds a function pointer into it.
    ~LoopState()
    {
        detachFromRootLoop();

        if (epollFd >= 0)
            ::close(epollFd);
    }

    void detachFromRootLoop()
    {
        if (auto* bridge = rootBridge.exchange(nullptr))
            bridge->detach(epollFd);
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

    // Root side: the hosted copies attached to this loop.
    std::mutex guestMutex;
    Vector<std::shared_ptr<RootLoopGuest>> guests;

    // Hosted side: the root copy's bridge, once attached to.
    std::atomic<RootLoopBridge*> rootBridge {nullptr};

    std::atomic<int> rootLoopDepth {0};
    bool advertisingRootLoop = false;
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

void pumpLoopState(LoopState& loop)
{
    if (loop.pumpDepth.load() > 0)
        return;

    auto scope = PumpScope {loop};
    pumpLoopOnce(loop);
}

// What the root copy calls through the bridge. The context is this copy's
// LoopState, so the round runs against this copy's own depth counter.
void pumpFromRootLoop(void* context)
{
    pumpLoopState(*static_cast<LoopState*>(context));
}

// Hosted side. The first thing that defers work in a copy living in a
// dynamic library hands the copy's one descriptor to whichever copy is
// running the process's root loop. Idempotent, and a no-op under a foreign
// host (a DAW), where nothing advertises a bridge and the plugin registers
// its own descriptor with the host instead.
void attachToRootLoop(LoopState& loop)
{
    if (loop.rootBridge.load() != nullptr || !Platform::isDLL() || !isMainThread())
        return;

    auto* bridge = findRootLoopBridge();

    if (bridge == nullptr || bridge == &rootLoopBridge)
        return;

    loop.hosted = true;
    loop.rootBridge = bridge;
    bridge->attach(loop.epollFd, pumpFromRootLoop, &loop);
}

// Root side. Another copy already running the root loop keeps it: a nested
// pump inside a hosted copy must never take the advertisement over.
void publishRootLoop(LoopState& loop)
{
    if (auto* other = findRootLoopBridge();
        other != nullptr && other != &rootLoopBridge)
        return;

    setEnv(rootLoopBridgeName, rootLoopBridgeAdvertisement());
    setEnv(rootLoopMarkerName, "1");
    loop.advertisingRootLoop = true;
}

void withdrawRootLoop(LoopState& loop)
{
    if (!loop.advertisingRootLoop)
        return;

    loop.advertisingRootLoop = false;
    setEnv(rootLoopMarkerName, "0");
    unsetEnv(rootLoopBridgeName);
}

// Held for the whole of run()/runFor, so a nested pump neither re-advertises
// nor withdraws the outermost loop's advertisement.
struct RootLoopScope
{
    explicit RootLoopScope(LoopState& loopToUse)
        : loop(loopToUse)
    {
        if (++loop.rootLoopDepth == 1)
            publishRootLoop(loop);
    }

    ~RootLoopScope()
    {
        if (--loop.rootLoopDepth == 0)
            withdrawRootLoop(loop);
    }

    RootLoopScope(const RootLoopScope&) = delete;
    RootLoopScope& operator=(const RootLoopScope&) = delete;

    LoopState& loop;
};

void forgetGuestLocked(LoopState& loop, int fd)
{
    for (const auto& guest: loop.guests)
        if (guest->fd == fd)
            guest->pump = nullptr;

    loop.guests.removeIndexesMatching(
        [fd](const std::shared_ptr<RootLoopGuest>& guest)
        { return guest->fd == fd; });
}

void rootLoopAttach(int fd, RootLoopPump pump, void* context)
{
    auto& loop = getLoop();

    auto guest = std::make_shared<RootLoopGuest>();
    guest->fd = fd;
    guest->context = context;
    guest->pump = pump;

    {
        auto lock = std::lock_guard(loop.guestMutex);
        forgetGuestLocked(loop, fd);
        loop.guests.add(guest);
    }

    // Readiness dispatches the guest's round; the prepare runs it again
    // before the root waits, which is the guest's chance to flush a display
    // connection it opened from a call the host made into it directly.
    auto pumpGuest = [guest]
    {
        if (auto run = guest->pump.load())
            run(guest->context);
    };

    addLoopSource(fd, POLLIN, pumpGuest, pumpGuest);
}

void rootLoopDetach(int fd)
{
    auto& loop = getLoop();

    {
        auto lock = std::lock_guard(loop.guestMutex);
        forgetGuestLocked(loop, fd);
    }

    removeLoopSource(fd);
}

void rootLoopStop()
{
    getEventLoop().quit();
}
} // namespace

int getEventLoopFd()
{
    return getLoop().epollFd;
}

void pumpEventLoop()
{
    pumpLoopState(getLoop());
}

void EventLoop::run()
{
    initMainThread();

    auto& loop = getLoop();
    loop.running = true;

    auto advertised = RootLoopScope {loop};
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

    auto advertised = RootLoopScope {loop};
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

    attachToRootLoop(loop);
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

    attachToRootLoop(loop);
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

    if (loop.running.load() || loop.hosted.load())
        return true;

    return findRootLoopBridge() != nullptr;
}

void attachCurrentThreadAsMain()
{
    initMainThread();

    auto& loop = getLoop();
    loop.hosted = true;
    attachToRootLoop(loop);
    loop.waker.wake();
}

void stopProcessRootLoop()
{
    if (auto* bridge = findRootLoopBridge())
        bridge->stop();
}

} // namespace eacp::Threads
