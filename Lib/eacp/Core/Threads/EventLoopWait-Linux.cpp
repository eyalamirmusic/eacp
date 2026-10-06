#include "EventLoopWait-Linux.h"

#include <cerrno>
#include <poll.h>

namespace eacp::Threads
{
WaitResult waitForLoopFd(int epollFd, int timeoutMs)
{
    auto fds = pollfd {epollFd, POLLIN, 0};
    auto r = ::poll(&fds, 1, timeoutMs);

    if (r > 0)
        return WaitResult::Ready;

    if (r == 0)
        return WaitResult::TimedOut;

    return errno == EINTR ? WaitResult::Ready : WaitResult::Failed;
}
} // namespace eacp::Threads
