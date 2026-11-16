#include "DisplayLink.h"

#include "AnimationFrame-Web.h"

#include <eacp/Core/Threads/EventLoop.h>

#include <memory>
#include <utility>

namespace eacp::Threads
{
namespace
{
// Shared with a queued tick, so one outliving the link touches no dead
// callback.
struct WebDisplayLinkTick
{
    explicit WebDisplayLinkTick(const Callback& cbToUse)
        : cb(cbToUse)
    {
    }

    Callback cb;
    bool alive = true;
    bool pending = false;
};
} // namespace

// The browser's own vsync, requestAnimationFrame, which stops while the page
// is hidden; under node a 60 Hz timeout. Each frame queues one tick, as the
// Linux pacing thread does, so the callback runs from the loop's queue and a
// slow one coalesces frames rather than stacking them.
struct DisplayLink::Native
{
    explicit Native(const Callback& cb)
        : state(std::make_shared<WebDisplayLinkTick>(cb))
    {
        requestNext();
    }

    ~Native() { state->alive = false; }

    void requestNext()
    {
        frame.request(
            [this]
            {
                postTick();
                requestNext();
            });
    }

    void postTick() const
    {
        if (std::exchange(state->pending, true))
            return;

        callAsync(
            [tick = state]
            {
                tick->pending = false;

                if (tick->alive)
                    tick->cb();
            });
    }

    std::shared_ptr<WebDisplayLinkTick> state;
    Graphics::AnimationFrame frame;
};

DisplayLink::DisplayLink(const FrameCallback& cb)
    : rateLimit(std::make_shared<RateLimit>())
    , callback(rateLimited(rateLimit, timedTick(cb)))
    , impl(callback)
{
}

} // namespace eacp::Threads
