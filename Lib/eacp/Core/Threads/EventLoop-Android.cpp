#include "EventLoop-Android.h"
#include "EventLoopWait-Linux.h"
#include "../Utils/Logging.h"

#include <android/looper.h>

// EventLoop-Linux.cpp's loop, waiting in the thread's ALooper rather than in
// poll(2): what Android delivers (the glue's commands, the input queue,
// choreographer frames) is registered with the looper, not exposed as
// descriptors. The loop's own epoll descriptor joins the looper as one more.

namespace eacp::Threads
{
namespace
{
// Above the glue's LOOPER_ID_MAIN, LOOPER_ID_INPUT and LOOPER_ID_USER.
constexpr int androidEventLoopIdent = 1000;

LooperEventHandler& androidLooperHandler()
{
    static auto handler = LooperEventHandler {[](int, void*) {}};
    return handler;
}

void androidWatchLoopFd(int epollFd)
{
    static auto* watchedBy = static_cast<ALooper*>(nullptr);

    auto* looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);

    if (looper == watchedBy)
        return;

    ALooper_addFd(looper,
                  epollFd,
                  androidEventLoopIdent,
                  ALOOPER_EVENT_INPUT,
                  nullptr,
                  nullptr);
    watchedBy = looper;
}
} // namespace

void setLooperEventHandler(LooperEventHandler handler)
{
    androidLooperHandler() =
        handler ? std::move(handler) : LooperEventHandler {[](int, void*) {}};
}

WaitResult waitForLoopFd(int epollFd, int timeoutMs)
{
    androidWatchLoopFd(epollFd);

    auto data = static_cast<void*>(nullptr);
    auto ident = ALooper_pollOnce(timeoutMs, nullptr, nullptr, &data);

    if (ident == ALOOPER_POLL_TIMEOUT)
        return WaitResult::TimedOut;

    if (ident == ALOOPER_POLL_ERROR)
    {
        LOG("EventLoop: ALooper_pollOnce failed");
        return WaitResult::Failed;
    }

    if (ident >= 0 && ident != androidEventLoopIdent)
        androidLooperHandler()(ident, data);

    return WaitResult::Ready;
}
} // namespace eacp::Threads
