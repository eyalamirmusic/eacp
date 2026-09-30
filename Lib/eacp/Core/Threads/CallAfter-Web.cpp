#include "EventLoop.h"

#include <emscripten/eventloop.h>

#include <memory>

// One browser timeout per pending callback: the browser already keeps the
// deadlines, so there is no scheduler thread to share.

namespace eacp::Threads
{
namespace
{
void webCallAfterFired(void* data)
{
    auto func = std::unique_ptr<Callback>(static_cast<Callback*>(data));
    (*func)();
}
} // namespace

void callAfter(Time::MS delay, Callback func)
{
    if (delay.count <= 0)
    {
        callAsync(func);
        return;
    }

    emscripten_set_timeout(
        webCallAfterFired, (double) delay.count, new Callback {std::move(func)});
}
} // namespace eacp::Threads
