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

// Per thread, as a looper is: a recreated activity's loop runs on a new thread
// whose looper may well be allocated where the last one was.
void androidWatchLoopFd(int epollFd)
{
    thread_local auto* watchedBy = static_cast<ALooper*>(nullptr);

    auto* looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);

    if (looper == watchedBy)
        return;

    if (ALooper_addFd(looper,
                      epollFd,
                      androidEventLoopIdent,
                      ALOOPER_EVENT_INPUT,
                      nullptr,
                      nullptr)
        != 1)
        LOG("EventLoop: ALooper_addFd failed; posted callbacks will wait for "
            "the next looper event");

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
