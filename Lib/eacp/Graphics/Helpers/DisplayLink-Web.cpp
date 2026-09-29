#include "DisplayLink.h"

#include <eacp/Core/Threads/ThreadUtils.h>

#include <emscripten/html5.h>

#include <memory>

// The browser's requestAnimationFrame, which is its vsync, and which it stops
// by itself while the tab is hidden.

namespace eacp::Threads
{
namespace
{
struct WebDisplayLinkTick
{
    explicit WebDisplayLinkTick(const Callback& cbToUse)
        : cb(cbToUse)
    {
    }

    Callback cb;
    bool alive = true;
};

using WebDisplayLinkTickPtr = std::shared_ptr<WebDisplayLinkTick>;

// The loop ends on the first frame after the link has gone.
bool webDisplayLinkFrame(double, void* data)
{
    auto* tick = static_cast<WebDisplayLinkTickPtr*>(data);

    if ((*tick)->alive)
        (*tick)->cb();

    if ((*tick)->alive)
        return true;

    delete tick;
    return false;
}
} // namespace

struct DisplayLink::Native
{
    explicit Native(const Callback& cb)
        : state(std::make_shared<WebDisplayLinkTick>(cb))
    {
        assertMainThread();
        emscripten_request_animation_frame_loop(webDisplayLinkFrame,
                                                new WebDisplayLinkTickPtr {state});
    }

    ~Native()
    {
        assertMainThread();
        state->alive = false;
    }

    WebDisplayLinkTickPtr state;
};

DisplayLink::DisplayLink(const FrameCallback& cb)
    : rateLimit(std::make_shared<RateLimit>())
    , callback(rateLimited(rateLimit, timedTick(cb)))
    , impl(callback)
{
}
} // namespace eacp::Threads
