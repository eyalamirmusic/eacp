#include "EventLoop.h"
#include "ThreadUtils-Linux.h"
#include "../Utils/Singleton.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <unistd.h>
#include <utility>
#include <vector>

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

struct LoopState
{
    PipeWaker waker;
    std::mutex mutex;
    std::vector<Callback> queue;
    std::atomic<bool> running {false};
};

static LoopState& getLoop()
{
    return Singleton::get<LoopState>();
}

namespace
{
void drainPending(LoopState& loop)
{
    auto pending = std::vector<Callback>();
    {
        auto lock = std::lock_guard(loop.mutex);
        pending.swap(loop.queue);
    }
    for (auto& cb: pending)
        cb();
}
} // namespace

void EventLoop::run()
{
    initMainThread();

    auto& loop = getLoop();
    loop.running = true;

    while (loop.running)
    {
        auto pfd = pollfd {loop.waker.readFd, POLLIN, 0};
        auto r = ::poll(&pfd, 1, -1);

        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }

        loop.waker.drain();
        drainPending(loop);
    }
}

bool EventLoop::runFor(std::chrono::milliseconds timeout)
{
    initMainThread();

    auto& loop = getLoop();
    loop.running = true;

    auto deadline = std::chrono::steady_clock::now() + timeout;
    auto timedOut = false;

    while (loop.running)
    {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            timedOut = true;
            break;
        }

        auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now)
                .count();

        auto pfd = pollfd {loop.waker.readFd, POLLIN, 0};
        auto r = ::poll(&pfd, 1, (int) remaining);

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
        loop.queue.push_back(std::move(func));
    }
    loop.waker.wake();
}

} // namespace eacp::Threads
