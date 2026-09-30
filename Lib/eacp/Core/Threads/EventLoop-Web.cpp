#include "EventLoop.h"
#include "ThreadUtils-Linux.h"
#include "../App/App.h"

#include <emscripten/emscripten.h>
#include <emscripten/eventloop.h>

// The browser owns the loop. run() hands the thread back to it and never
// returns to its caller; queued work runs in browser tasks of its own, and the
// timers, input and frames are the browser's too. There is one thread, so the
// queue needs no lock.

namespace eacp::Threads
{
namespace
{
struct WebLoop
{
    Vector<Callback> queue;
    bool drainPosted = false;
    bool running = false;

    // run() was entered, so quitting is the app's end, not a nested pump's.
    bool rootEntered = false;
};

WebLoop& webLoop()
{
    static auto loop = WebLoop {};
    return loop;
}

// A MessageChannel task: unlike setTimeout(0) it is never clamped to 4 ms, and
// unlike a microtask it lets the browser deliver input and draw between drains.
EM_JS(void, webPostDrain, (), {
    if (!Module.eacpDrainChannel)
    {
        var channel = new MessageChannel();
        channel.port1.onmessage = function()
        {
            _eacpWebDrainEventLoop();
        };
        Module.eacpDrainChannel = channel;
    }

    Module.eacpDrainChannel.port2.postMessage(0);
});

void webPostDrainOnce(WebLoop& loop)
{
    if (loop.drainPosted)
        return;

    loop.drainPosted = true;
    webPostDrain();
}

// What a callback queues lands in the next round, so a callback that queues
// itself cannot hold the thread.
bool webDrainRound(WebLoop& loop)
{
    if (loop.queue.empty())
        return false;

    auto pending = std::move(loop.queue);
    loop.queue.clear();

    for (auto& cb: pending)
        cb();

    return true;
}

// run<T> destroys the app once its loop has exited, and on the web its loop
// never returns to it, so quitting does that here, in a task of its own rather
// than inside whatever callback asked to quit.
void webTearDown(void*)
{
    Apps::destroyApp();
}
} // namespace

extern "C" EMSCRIPTEN_KEEPALIVE void eacpWebDrainEventLoop()
{
    auto& loop = webLoop();
    loop.drainPosted = false;

    webDrainRound(loop);
}

void EventLoop::run()
{
    initMainThread();

    auto& loop = webLoop();
    loop.running = true;
    loop.rootEntered = true;

    if (!loop.queue.empty())
        webPostDrainOnce(loop);

    emscripten_unwind_to_js_event_loop();
}

bool EventLoop::runFor(Time::MS timeout)
{
    initMainThread();

    auto& loop = webLoop();
    auto wasRunning = loop.running;
    auto deadline = Time::Deadline {timeout};

    loop.running = true;

    while (loop.running && !deadline.expired() && webDrainRound(loop))
    {
    }

    if (!loop.running)
        return true;

    loop.running = wasRunning;
    return false;
}

void EventLoop::quit()
{
    auto& loop = webLoop();
    loop.running = false;

    if (!loop.rootEntered)
        return;

    loop.rootEntered = false;
    emscripten_async_call(webTearDown, nullptr, 0);
}

void stopEventLoop()
{
    getEventLoop().quit();
}

void EventLoop::call(Callback func)
{
    auto& loop = webLoop();
    loop.queue.add(std::move(func));

    webPostDrainOnce(loop);
}

void scheduleStartup(const Callback& func)
{
    callAsync(func);
}

bool isEventLoopRunning()
{
    return webLoop().running;
}

void attachCurrentThreadAsMain()
{
    initMainThread();
}

// One copy of eacp per page, so the root loop is this one.
void stopProcessRootLoop()
{
    getEventLoop().quit();
}
} // namespace eacp::Threads
