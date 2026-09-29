#pragma once

namespace eacp::Threads
{
enum class WaitResult
{
    Ready,
    TimedOut,
    Failed
};

WaitResult waitForLoopFd(int epollFd, int timeoutMs);
} // namespace eacp::Threads
