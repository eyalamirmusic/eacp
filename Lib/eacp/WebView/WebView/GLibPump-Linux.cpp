#include "GLibPump-Linux.h"

#include <eacp/Core/Core.h>
#include <eacp/Core/Threads/EventLoop-Linux.h>
#include <eacp/Core/Utils/Singleton.h>

#include <glib.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <utility>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <vector>

namespace eacp::Graphics::detail
{
namespace
{
class GLibPump
{
public:
    GLibPump()
        : context(g_main_context_default())
    {
        Threads::assertMainThread();

        if (!g_main_context_acquire(context))
        {
            LOG("WebView: GLib's main context belongs to another thread, so "
                "nothing will pump it");
            return;
        }

        epollFd = epoll_create1(EPOLL_CLOEXEC);
        timerFd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

        if (epollFd < 0 || timerFd < 0)
        {
            LOG("WebView: cannot create the descriptors that pump GLib");
            return;
        }

        auto timerEvent = epoll_event {};
        timerEvent.events = EPOLLIN;
        timerEvent.data.fd = timerFd;
        epoll_ctl(epollFd, EPOLL_CTL_ADD, timerFd, &timerEvent);

        Threads::addLoopSource(
            epollFd,
            POLLIN,
            [this] { finishCycle(); },
            [this]
            {
                finishCycle();
                beginCycle();
            });

        running = true;
    }

    bool isRunning() const { return running; }

private:
    void beginCycle()
    {
        auto ready = g_main_context_prepare(context, &maxPriority) != FALSE;
        auto timeout = -1;

        fds.resize(std::max<std::size_t>(fds.capacity(), 8));

        for (;;)
        {
            auto count = g_main_context_query(
                context, maxPriority, &timeout, fds.data(), (gint) fds.size());

            if (count <= (gint) fds.size())
            {
                fds.resize((std::size_t) count);
                break;
            }

            fds.resize((std::size_t) count);
        }

        watch();
        armTimer(ready ? 0 : timeout);
        cyclePending = true;
    }

    void finishCycle()
    {
        drainTimer();

        if (!std::exchange(cyclePending, false))
            return;

        if (!fds.empty())
            g_poll(fds.data(), (guint) fds.size(), 0);

        if (g_main_context_check(
                context, maxPriority, fds.data(), (gint) fds.size()))
            g_main_context_dispatch(context);
    }

    void watch()
    {
        auto wanted = std::vector<std::pair<int, std::uint32_t>> {};

        for (auto& fd: fds)
        {
            auto found =
                std::find_if(wanted.begin(),
                             wanted.end(),
                             [&](auto& entry) { return entry.first == fd.fd; });

            if (found == wanted.end())
                wanted.emplace_back(fd.fd, (std::uint32_t) fd.events);
            else
                found->second |= (std::uint32_t) fd.events;
        }

        for (auto& [fd, events]: watched)
        {
            auto stillWanted =
                std::any_of(wanted.begin(),
                            wanted.end(),
                            [fd](auto& entry) { return entry.first == fd; });

            if (!stillWanted)
                epoll_ctl(epollFd, EPOLL_CTL_DEL, fd, nullptr);
        }

        // Every wanted fd is modified, not only the changed ones: a descriptor
        // GLib closed and reopened under the same number has silently left the
        // epoll set, and only a MOD failing with ENOENT says so.
        for (auto& [fd, events]: wanted)
        {
            auto event = epoll_event {};
            event.events = events;
            event.data.fd = fd;

            if (epoll_ctl(epollFd, EPOLL_CTL_MOD, fd, &event) != 0
                && errno == ENOENT)
                epoll_ctl(epollFd, EPOLL_CTL_ADD, fd, &event);
        }

        watched = std::move(wanted);
    }

    void armTimer(int timeoutMs)
    {
        auto spec = itimerspec {};

        if (timeoutMs == 0)
            spec.it_value.tv_nsec = 1;
        else if (timeoutMs > 0)
        {
            spec.it_value.tv_sec = timeoutMs / 1000;
            spec.it_value.tv_nsec = (long) (timeoutMs % 1000) * 1000000L;
        }

        timerfd_settime(timerFd, 0, &spec, nullptr);
    }

    void drainTimer()
    {
        auto expirations = std::uint64_t {};

        while (read(timerFd, &expirations, sizeof(expirations)) > 0)
        {
        }
    }

    GMainContext* context = nullptr;
    int epollFd = -1;
    int timerFd = -1;
    gint maxPriority = 0;
    bool cyclePending = false;
    bool running = false;
    std::vector<GPollFD> fds;
    std::vector<std::pair<int, std::uint32_t>> watched;
};
} // namespace

bool ensureGLibPump()
{
    return Singleton::getImmortal<GLibPump>().isRunning();
}
} // namespace eacp::Graphics::detail
